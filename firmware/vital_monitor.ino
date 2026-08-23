/*
 * VitalSync - Vital Sign Monitor Firmware
 * 
 * Reads temperature (MLX90614), heart rate (MAX30102), and respiration rate
 * (flex sensor) from non-invasive sensors. Displays on LCD, triggers buzzer
 * alerts, and outputs serial data for cloud transmission.
 * 
 * Board: Arduino Uno R4 WiFi
 * Baud:  115200
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MLX90614.h>
#include "MAX30105.h"

// --- Peripherals ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
MAX30105 particleSensor;

// --- Pin Definitions ---
#define FLEX_PIN A0
#define BUZZER   8

// --- Heart Rate Detection ---
long prevIR = 0;
long smoothDiff = 0;
bool pulseHigh = false;
unsigned long lastBeatMs = 0;
unsigned long lastValidHRMs = 0;
bool wasFinger = false;

const byte RATE_SIZE = 8;
int rates[RATE_SIZE];
byte rateSpot = 0;
int beatAvg = 0;

// --- Respiration Detection ---
int flexBaseline = 0;
bool inhaling = false;
unsigned long lastBreathMs = 0;
unsigned long breathIntervals[5] = {0, 0, 0, 0, 0};
byte breathIdx = 0;
int respRate = 0;
unsigned long lastValidRRMs = 0;

// --- Timing ---
unsigned long lastReportMs = 0;

// --- Alert Thresholds ---
const float TEMP_HIGH = 37.8;
const float TEMP_LOW  = 30.0;
const int HR_HIGH = 120;
const int HR_LOW  = 50;
const int RR_HIGH = 25;
const int RR_LOW  = 8;

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, HIGH); // OFF (low-level trigger)

  delay(2000);
  Serial.println("=== VitalSync Monitor ===");
  Wire.begin();

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.print("VitalSync");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  // Initialize MLX90614 (must be first to set I2C speed before MAX30102)
  if (!mlx.begin()) {
    Serial.println("MLX FAIL");
    lcd.clear();
    lcd.print("MLX FAIL");
    while (1);
  }
  Serial.println("MLX OK");

  // Initialize MAX30102 at standard I2C speed (100 kHz)
  // Using 400 kHz causes MLX90614 to return NaN
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX FAIL");
    lcd.clear();
    lcd.print("MAX FAIL");
    while (1);
  }
  // Low LED power (0x06) prevents ADC saturation at 262143
  particleSensor.setup(0x06, 8, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x06);
  particleSensor.setPulseAmplitudeIR(0x06);
  particleSensor.setPulseAmplitudeGreen(0);
  Serial.println("MAX OK");

  // Warm up MAX30102 (stabilize IR readings)
  for (int i = 0; i < 100; i++) {
    prevIR = particleSensor.getIR();
    delay(10);
  }

  // Calibrate flex sensor baseline (keep sensor flat during this phase)
  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += analogRead(FLEX_PIN);
    delay(20);
  }
  flexBaseline = sum / 50;
  Serial.print("Flex baseline: ");
  Serial.println(flexBaseline);

  // Buzzer self-test (two short beeps)
  digitalWrite(BUZZER, LOW);
  delay(100);
  digitalWrite(BUZZER, HIGH);
  delay(100);
  digitalWrite(BUZZER, LOW);
  delay(100);
  digitalWrite(BUZZER, HIGH);
  Serial.println("BUZZER OK");

  lcd.clear();
  lcd.print("Ready!");
  delay(1000);
  lcd.clear();
}

// Reset heart rate buffer when finger is removed and replaced
void resetHR() {
  beatAvg = 0;
  rateSpot = 0;
  lastBeatMs = 0;
  lastValidHRMs = 0;
  smoothDiff = 0;
  pulseHigh = false;
  for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
}

void loop() {
  // --------------------------------------------------
  // Heart Rate: polled every iteration (no delay)
  // Uses derivative-based beat detection to handle
  // IR baseline drift that breaks library detection
  // --------------------------------------------------
  long irValue = particleSensor.getIR();
  long diff = irValue - prevIR;
  prevIR = irValue;
  smoothDiff = (smoothDiff * 3 + diff) / 4; // Exponential moving average

  bool finger = irValue > 10000;

  // Reset buffer on new finger placement
  if (finger && !wasFinger) {
    resetHR();
    for (int i = 0; i < 50; i++) {
      prevIR = particleSensor.getIR();
      delay(10);
    }
  }
  wasFinger = finger;

  if (finger) {
    // Beat onset: smoothed derivative crosses threshold
    if (smoothDiff > 40 && !pulseHigh) {
      pulseHigh = true;
      unsigned long now = millis();
      unsigned long delta = now - lastBeatMs;

      // Reject physiologically impossible intervals
      if (lastBeatMs > 0 && delta > 300 && delta < 1500) {
        int bpm = 60000 / delta;
        if (bpm > 40 && bpm < 200) {
          rates[rateSpot++] = bpm;
          rateSpot %= RATE_SIZE;

          // Compute running average
          long s = 0;
          byte count = 0;
          for (byte i = 0; i < RATE_SIZE; i++) {
            if (rates[i] > 0) { s += rates[i]; count++; }
          }
          if (count > 0) beatAvg = s / count;
          lastValidHRMs = now;
        }
      }
      lastBeatMs = now;
    }
    // Hysteresis: wait for signal to drop before next beat
    if (smoothDiff < -10) pulseHigh = false;
  }

  // Clear HR display if no valid beat for 5 seconds
  if (lastValidHRMs > 0 && millis() - lastValidHRMs > 5000) {
    beatAvg = 0;
  }

  // --------------------------------------------------
  // Respiration: flex sensor with bidirectional detection
  // Uses abs(reading - baseline) to catch bends in either direction
  // --------------------------------------------------
  int flex = analogRead(FLEX_PIN);
  int flexDiff = abs(flex - flexBaseline);

  if (!inhaling && flexDiff > 3) {
    inhaling = true;
    unsigned long now = millis();
    if (lastBreathMs > 0) {
      unsigned long interval = now - lastBreathMs;
      if (interval > 1500 && interval < 15000) {
        breathIntervals[breathIdx++] = interval;
        breathIdx %= 5;

        // Average last 5 breath intervals
        unsigned long total = 0;
        byte n = 0;
        for (byte i = 0; i < 5; i++) {
          if (breathIntervals[i] > 0) { total += breathIntervals[i]; n++; }
        }
        if (n > 0) respRate = 60000UL / (total / n);
        lastValidRRMs = now;
      }
    }
    lastBreathMs = now;
  }
  if (inhaling && flexDiff < 2) inhaling = false;

  // Clear RR if no breath detected for 10 seconds
  if (lastValidRRMs > 0 && millis() - lastValidRRMs > 10000) {
    respRate = 0;
    lastValidRRMs = 0;
  }

  // --------------------------------------------------
  // Report: every 1 second
  // Updates LCD, checks alerts, outputs serial data
  // --------------------------------------------------
  if (millis() - lastReportMs >= 1000) {
    lastReportMs = millis();
    float tempC = mlx.readObjectTempC();

    // Alert evaluation
    bool alert = false;
    if (tempC > TEMP_HIGH || (tempC < TEMP_LOW && tempC > 30)) alert = true;
    if (beatAvg > 0 && (beatAvg > HR_HIGH || beatAvg < HR_LOW)) alert = true;
    if (respRate > 0 && (respRate > RR_HIGH || respRate < RR_LOW)) alert = true;
    digitalWrite(BUZZER, alert ? LOW : HIGH);

    // LCD line 1: temperature and heart rate
    lcd.setCursor(0, 0);
    lcd.print("T:");
    lcd.print(tempC, 1);
    lcd.print((char)223);
    lcd.print(" HR:");
    if (beatAvg > 0) {
      lcd.print(beatAvg);
      lcd.print("  ");
    } else if (finger) {
      lcd.print(".. ");
    } else {
      lcd.print("-- ");
    }

    // LCD line 2: respiration rate and status
    lcd.setCursor(0, 1);
    lcd.print("RR:");
    if (respRate > 0) {
      lcd.print(respRate);
      lcd.print(" ");
    } else {
      lcd.print("-- ");
    }
    lcd.print(alert ? "ALERT " : "OK    ");

    // Serial output (parsed by bridge script)
    Serial.print("T:");
    Serial.print(tempC, 1);
    Serial.print(" HR:");
    Serial.print(beatAvg);
    Serial.print(" RR:");
    Serial.print(respRate);
    Serial.print(" A:");
    Serial.println(alert ? 1 : 0);
  }
}
