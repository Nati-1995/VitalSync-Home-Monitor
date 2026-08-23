/*
 * VitalSync - Serial to Firebase Bridge
 * 
 * Reads vital sign data from Arduino serial output and pushes
 * it to Firebase Realtime Database via REST API.
 * 
 * Usage: node bridge.js
 * Requires: npm install serialport
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('serialport');

// Configuration
const SERIAL_PORT = 'COM19';        // Update to match your Arduino port
const BAUD_RATE = 115200;
const DB_URL = 'https://YOUR_PROJECT-default-rtdb.firebaseio.com'; // Replace with your Firebase URL

const port = new SerialPort({ path: SERIAL_PORT, baudRate: BAUD_RATE });
const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));

console.log(`VitalSync Bridge - Listening on ${SERIAL_PORT}...`);

parser.on('data', async (line) => {
  line = line.trim();
  if (!line.startsWith('T:')) return;

  // Parse serial format: T:23.5 HR:75 RR:16 A:0
  const parts = {};
  line.split(' ').forEach(p => {
    const [key, val] = p.split(':');
    parts[key] = isNaN(val) ? val : parseFloat(val);
  });

  const data = {
    temperature: parts.T || 0,
    heartRate: parts.HR || 0,
    respRate: parts.RR || 0,
    alert: parts.A === 1,
    timestamp: Date.now()
  };

  console.log(`T:${data.temperature} HR:${data.heartRate} RR:${data.respRate} Alert:${data.alert}`);

  try {
    // Update current reading
    await fetch(`${DB_URL}/current.json`, {
      method: 'PUT',
      body: JSON.stringify(data)
    });

    // Append to history log
    await fetch(`${DB_URL}/history.json`, {
      method: 'POST',
      body: JSON.stringify(data)
    });
  } catch (err) {
    console.error('Firebase error:', err.message);
  }
});

port.on('error', (err) => console.error('Serial error:', err.message));
