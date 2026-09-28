const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const cors = require('cors');
const axios = require('axios');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

app.use(cors());
app.use(express.json());

// ── Phase 1 state ──
let clients = new Set();
let latestGPSData = null;
let deviceStatus = {
  gpsConnected: false,
  obstacleDetected: false,
  distance: null,
  sensors: null,
  satellites: 0,
  lastSeen: null
};

// ── Phase 2 state (camera) ──
let camClients = new Set();       // dashboards waiting for the next captured frame
let esp32CamSocket = null;        // the ESP32-CAM's own WebSocket connection
let pendingFrameMeta = null;      // { size, width, height } until binary arrives

function broadcast(data) {
  const msg = JSON.stringify(data);
  clients.forEach(ws => {
    if (ws.readyState === WebSocket.OPEN) ws.send(msg);
  });
}

wss.on('connection', (ws, req) => {
  const isESP32     = req.url === '/esp32';
  const isDashboard = req.url === '/dashboard';
  const isESP32Cam  = req.url === '/esp32cam';

  console.log(`New connection: ${isESP32 ? 'ESP32' : isESP32Cam ? 'ESP32-CAM' : 'Dashboard'}`);

  if (isDashboard) {
    clients.add(ws);
    ws.send(JSON.stringify({ type: 'status', data: deviceStatus }));
    if (latestGPSData) ws.send(JSON.stringify({ type: 'gps', data: latestGPSData }));
    ws.send(JSON.stringify({ type: 'cam_status', online: !!(esp32CamSocket && esp32CamSocket.readyState === WebSocket.OPEN) }));
  }

  if (isESP32Cam) {
    esp32CamSocket = ws;
    console.log('[CAM] ESP32-CAM connected');
    broadcast({ type: 'cam_status', online: true }); // not strictly needed but harmless
  }

  ws.on('message', (raw, isBinary) => {

    // ── Binary JPEG frame coming back from ESP32-CAM ──
    if (isESP32Cam && isBinary) {
      console.log(`[CAM] Received binary frame: ${raw.length} bytes`);
      camClients.forEach(dash => {
        if (dash.readyState === WebSocket.OPEN) {
          if (pendingFrameMeta) {
            dash.send(JSON.stringify({ type: 'cam_frame_meta', ...pendingFrameMeta }));
          }
          dash.send(raw, { binary: true });
        }
      });
      camClients.clear();
      pendingFrameMeta = null;
      return;
    }

    // ── Text/JSON messages ──
    try {
      const msg = JSON.parse(raw.toString());

      if (isESP32Cam) {
        switch (msg.type) {
          case 'frame_meta':
            pendingFrameMeta = { size: msg.size, width: msg.width, height: msg.height };
            break;
          case 'status':
            console.log('[CAM] Status:', msg.data);
            break;
          case 'error':
            console.log('[CAM] Error:', msg.reason);
            camClients.forEach(dash => {
              if (dash.readyState === WebSocket.OPEN) {
                dash.send(JSON.stringify({ type: 'cam_error', reason: msg.reason }));
              }
            });
            camClients.clear();
            break;
        }
        return;
      }

      if (isDashboard) {
        if (msg.type === 'request_capture') {
          if (!esp32CamSocket || esp32CamSocket.readyState !== WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: 'cam_error', reason: 'camera_offline' }));
            return;
          }
          camClients.add(ws);
          esp32CamSocket.send(JSON.stringify({ command: 'capture' }));
          console.log('[CAM] Capture requested by dashboard');
          return;
        }
      }

      if (isESP32) {
        switch (msg.type) {
          case 'gps':
            latestGPSData = { ...msg.data, timestamp: Date.now() };
            deviceStatus.gpsConnected = true;
            deviceStatus.satellites = msg.data.satellites || 0;
            deviceStatus.lastSeen = Date.now();
            broadcast({ type: 'gps', data: latestGPSData });
            break;

          case 'obstacle':
            deviceStatus.obstacleDetected = msg.detected || (msg.sensors && Object.values(msg.sensors).some(s => s.detected));
            deviceStatus.distance = msg.distance || null;
            deviceStatus.sensors  = msg.sensors  || null;
            broadcast({ type: 'obstacle', detected: msg.detected, distance: msg.distance, sensors: msg.sensors });
            break;

          case 'status':
            deviceStatus = { ...deviceStatus, ...msg.data };
            broadcast({ type: 'status', data: deviceStatus });
            break;
        }
      }
    } catch (e) {
      console.error('Parse error:', e.message);
    }
  });

  ws.on('close', () => {
    clients.delete(ws);
    camClients.delete(ws);

    if (isESP32Cam && esp32CamSocket === ws) {
      esp32CamSocket = null;
      console.log('[CAM] ESP32-CAM disconnected');
      broadcast({ type: 'cam_status', online: false });
    }

    if (isESP32) {
      deviceStatus.gpsConnected = false;
      broadcast({ type: 'status', data: deviceStatus });
      console.log('ESP32 disconnected');
    }
  });
});

// ════════════════════════════════════════════
// REST API Routes
// ════════════════════════════════════════════

app.post('/api/gps', (req, res) => {
  const { lat, lng, speed, course, satellites, hdop } = req.body;
  if (!lat || !lng) return res.status(400).json({ error: 'lat and lng required' });

  latestGPSData = { lat, lng, speed, course, satellites, hdop, timestamp: Date.now() };
  deviceStatus.gpsConnected = true;
  deviceStatus.satellites = satellites || 0;
  deviceStatus.lastSeen = Date.now();

  broadcast({ type: 'gps', data: latestGPSData });
  res.json({ success: true });
});

app.post('/api/obstacle', (req, res) => {
  const { detected, distance, sensors } = req.body;
  deviceStatus.obstacleDetected = detected || (sensors && Object.values(sensors).some(s => s.detected));
  deviceStatus.distance = distance || null;
  deviceStatus.sensors  = sensors  || null;
  broadcast({ type: 'obstacle', detected, distance, sensors });
  res.json({ success: true });
});

app.get('/api/status', (req, res) => {
  res.json({ status: deviceStatus, gps: latestGPSData });
});

app.get('/api/camera-status', (req, res) => {
  res.json({ online: !!(esp32CamSocket && esp32CamSocket.readyState === WebSocket.OPEN) });
});

app.get('/api/search', async (req, res) => {
  const { q, lat, lng } = req.query;
  if (!q) return res.status(400).json({ error: 'Query required' });

  try {
    const params = { q, format: 'json', limit: 5, addressdetails: 1 };
    if (lat && lng) {
      params.viewbox = `${parseFloat(lng) - 0.1},${parseFloat(lat) + 0.1},${parseFloat(lng) + 0.1},${parseFloat(lat) - 0.1}`;
      params.bounded = 0;
    }

    const response = await axios.get('https://nominatim.openstreetmap.org/search', {
      params,
      headers: { 'User-Agent': 'BlindNav/1.0' }
    });

    const results = response.data.map(p => ({
      id: p.place_id,
      name: p.display_name.split(',')[0],
      fullName: p.display_name,
      lat: parseFloat(p.lat),
      lng: parseFloat(p.lon),
      type: p.type,
      category: p.class
    }));

    res.json({ results });
  } catch (err) {
    console.error('Search error:', err.message);
    res.status(500).json({ error: 'Search failed' });
  }
});

app.get('/api/route', async (req, res) => {
  const { fromLat, fromLng, toLat, toLng } = req.query;
  if (!fromLat || !fromLng || !toLat || !toLng) {
    return res.status(400).json({ error: 'All coordinates required' });
  }

  try {
    const url = `https://router.project-osrm.org/route/v1/walking/${fromLng},${fromLat};${toLng},${toLat}?overview=full&geometries=geojson&steps=true&annotations=true`;
    const response = await axios.get(url);

    if (response.data.code !== 'Ok') {
      return res.status(400).json({ error: 'No route found' });
    }

    const route = response.data.routes[0];
    const steps = route.legs[0].steps.map(step => ({
      instruction: step.maneuver.type,
      modifier: step.maneuver.modifier,
      distance: step.distance,
      duration: step.duration,
      name: step.name,
      location: step.maneuver.location
    }));

    res.json({ distance: route.distance, duration: route.duration, geometry: route.geometry, steps });
  } catch (err) {
    console.error('Route error:', err.message);
    res.status(500).json({ error: 'Routing failed' });
  }
});

app.get('/api/nearby', async (req, res) => {
  const { lat, lng, type } = req.query;
  if (!lat || !lng || !type) return res.status(400).json({ error: 'lat, lng, type required' });

  const typeMap = {
    'petrol pump': 'fuel', 'hospital': 'hospital', 'pharmacy': 'pharmacy',
    'police': 'police', 'restaurant': 'restaurant', 'bank': 'bank',
    'atm': 'atm', 'bus stop': 'bus_stop', 'school': 'school'
  };
  const osmType = typeMap[type.toLowerCase()] || type;

  try {
    const query = `[out:json][timeout:25];
      node["amenity"="${osmType}"](around:2000,${lat},${lng});
      out body 5;`;

    const response = await axios.post('https://overpass-api.de/api/interpreter', query, {
      headers: { 'Content-Type': 'text/plain' }
    });

    const results = (response.data.elements || []).map(el => ({
      id: el.id,
      name: el.tags?.name || osmType,
      lat: el.lat,
      lng: el.lon,
      type: osmType,
      tags: el.tags
    }));

    res.json({ results });
  } catch (err) {
    console.error('Nearby error:', err.message);
    res.status(500).json({ error: 'Nearby search failed' });
  }
});

app.post('/api/voice-command', async (req, res) => {
  const { command, lat, lng } = req.body;
  if (!command) return res.status(400).json({ error: 'Command required' });

  const cmd = command.toLowerCase().trim();
  let response = { action: 'unknown', message: 'Command not recognized' };

  const nearestMatch = cmd.match(/(?:find|show|map|navigate to|go to|take me to)?\s*(?:nearest|nearby|closest)?\s*(.+)/i);
  const navigateMatch = cmd.match(/(?:navigate to|go to|take me to|directions to)\s+(.+)/i);

  // ── Phase 2: "what's ahead" / object detection trigger ──
  const visionTriggers = [
    'what is ahead', "what's ahead", 'what is in front', "what's in front",
    'what do you see', 'describe surroundings', 'what is around me',
    'look ahead', 'scan ahead', 'identify object', 'what is this'
  ];

  if (visionTriggers.some(t => cmd.includes(t))) {
    response = { action: 'capture_vision', message: 'Capturing image to identify objects ahead' };
  } else if (cmd.includes('where am i') || cmd.includes('my location') || cmd.includes('current location')) {
    response = { action: 'show_location', message: 'Showing your current location' };
  } else if (cmd.includes('obstacle') || cmd.includes('distance')) {
    response = { action: 'obstacle_info', message: 'Checking obstacle sensor' };
  } else if (cmd.includes('stop navigation') || cmd.includes('cancel route') || cmd.includes('stop')) {
    response = { action: 'stop_navigation', message: 'Navigation stopped' };
  } else if (navigateMatch) {
    const destination = navigateMatch[1].trim();
    response = { action: 'navigate', destination, message: `Searching for ${destination}` };
  } else if (cmd.includes('nearest') || cmd.includes('nearby') || cmd.includes('closest')) {
    const placeTypes = ['petrol pump', 'hospital', 'pharmacy', 'police', 'restaurant', 'bank', 'atm', 'bus stop', 'school'];
    const foundType = placeTypes.find(t => cmd.includes(t));
    if (foundType) {
      response = { action: 'find_nearby', placeType: foundType, message: `Finding nearest ${foundType}` };
    } else if (nearestMatch) {
      response = { action: 'navigate', destination: nearestMatch[1], message: `Searching for ${nearestMatch[1]}` };
    }
  } else if (nearestMatch && nearestMatch[1].length > 2) {
    response = { action: 'navigate', destination: nearestMatch[1], message: `Searching for ${nearestMatch[1]}` };
  }

  res.json(response);
});

const PORT = process.env.PORT || 3001;
server.listen(PORT, () => {
  console.log(`BlindNav Server running on port ${PORT}`);
});
