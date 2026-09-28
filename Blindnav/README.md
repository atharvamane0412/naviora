# 🧭 BlindNav — Phase 1: GPS Navigation System for Visually Impaired

A complete IoT navigation system combining an ESP32 hardware device with a real-time web dashboard, designed to assist visually impaired users navigate independently.

---

## 📦 Project Structure

```
blindnav/
├── backend/          → Node.js + WebSocket server (deploy to Render)
├── frontend/         → Interactive Leaflet.js dashboard (deploy to Render)
└── esp32/            → Arduino firmware for ESP32 device
```

---

## 🔧 Hardware (Phase 1)

| Component | Purpose | Connection |
|---|---|---|
| ESP32 DevKit | Main controller + WiFi | — |
| NEO-6M GPS | Location data | UART2 (RX=16, TX=17) |
| HC-SR04 Ultrasonic | Obstacle detection | TRIG=5, ECHO=18 |
| Buzzer | Audio alerts | GPIO 26 |

### Wiring Diagram

```
ESP32          NEO-6M GPS
GPIO16 (RX2) ← TX
GPIO17 (TX2) → RX
3.3V         → VCC
GND          → GND

ESP32          HC-SR04
GPIO5        → TRIG
GPIO18       ← ECHO
5V           → VCC
GND          → GND

ESP32          Buzzer
GPIO26       → + (positive)
GND          → - (negative)
```

---

## 🚀 Deployment

### Step 1: Deploy Backend to Render

1. Create a GitHub repo and push the `backend/` folder
2. Go to [render.com](https://render.com) → New → Web Service
3. Connect your GitHub repo
4. Settings:
   - **Name**: `blindnav-backend`
   - **Build Command**: `npm install`
   - **Start Command**: `node server.js`
   - **Environment**: Node
5. Deploy → Copy your URL (e.g., `https://blindnav-backend.onrender.com`)

### Step 2: Deploy Frontend to Render

1. Push `frontend/` to GitHub (same or separate repo)
2. New → Web Service
3. Settings:
   - **Name**: `blindnav-frontend`
   - **Build Command**: `npm install`
   - **Start Command**: `node server.js`
4. Set environment variable: `REACT_APP_BACKEND_URL=https://blindnav-backend.onrender.com`
5. After deploying, edit `frontend/public/app.js` line 8:
   ```js
   : 'https://blindnav-backend.onrender.com', // your actual URL
   ```

### Step 3: Flash ESP32

#### Using Arduino IDE:
1. Install libraries:
   - `TinyGPS++` by Mikal Hart
   - `WebSockets` by Markus Sattler
   - `ArduinoJson` by Benoit Blanchon
2. Open `esp32/blindnav_esp32.ino`
3. Edit credentials:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* WS_HOST       = "blindnav-backend.onrender.com";
   ```
4. Select Board: `ESP32 Dev Module`
5. Upload

#### Using PlatformIO (recommended):
```bash
cd esp32
pio run --target upload
pio device monitor  # View serial output
```

---

## 🎙️ Voice Commands

| Say | Action |
|---|---|
| "Where am I" / "My location" | Center map on current GPS position |
| "Navigate to [place name]" | Search and route to destination |
| "Nearest hospital" | Find & navigate to closest hospital |
| "Nearest petrol pump" | Find nearest fuel station |
| "Nearest pharmacy" | Find nearest pharmacy |
| "Nearest police" | Find nearest police station |
| "Stop navigation" / "Cancel route" | Cancel active navigation |
| "What is ahead" | Check obstacle sensor reading |

Press **Space** or tap 🎙️ to activate voice input.

---

## 🔔 Buzzer Patterns

| Pattern | Meaning |
|---|---|
| Double beep (slow) | Searching for GPS satellites |
| 3 quick rising beeps | GPS fixed — navigation ready |
| Silent | GPS fixed, no obstacle |
| Medium beep (600ms cycle) | Obstacle detected < 80cm |
| Rapid beep (150ms cycle) | Danger! Obstacle < 30cm |
| 2 long descending beeps | Wrong route / off route |

---

## 📡 API Reference

### REST Endpoints

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/status` | Device status |
| POST | `/api/gps` | Receive GPS data (HTTP fallback) |
| POST | `/api/obstacle` | Receive obstacle data |
| GET | `/api/search?q=&lat=&lng=` | Search for places |
| GET | `/api/route?fromLat=&fromLng=&toLat=&toLng=` | Get walking route |
| GET | `/api/nearby?lat=&lng=&type=` | Find nearby POIs |
| POST | `/api/voice-command` | Process voice command text |

### WebSocket

- **ESP32** connects to: `wss://your-backend.onrender.com/esp32`
- **Dashboard** connects to: `wss://your-backend.onrender.com/dashboard`

ESP32 sends JSON frames:
```json
// GPS update
{"type":"gps","data":{"lat":18.5204,"lng":73.8567,"speed":3.2,"satellites":8,"hdop":1.2}}

// Obstacle
{"type":"obstacle","detected":true,"distance":45.3}

// Status
{"type":"status","data":{"gpsConnected":true,"obstacleDetected":false}}
```

---

## 🗺️ Dashboard Features

- **Real-time GPS tracking** on Leaflet map (dark/light themes)
- **Voice commands** with visual waveform feedback
- **Text-to-speech** audio guidance
- **Walking route** with turn-by-turn instructions
- **Off-route detection** with auto-recalculate
- **Quick POI buttons**: hospital, petrol, pharmacy, police, ATM, food, bus, bank
- **Device monitor**: distance, satellites, speed, accuracy rings
- **Buzzer status** with animated indicator
- **Activity log** with timestamps
- **Manual GPS input** for testing
- **Right-click map** to set custom destination
- **GPS simulation** button for demo

---

## 🔭 Phase 2 Roadmap

Phase 2 will add:
- **ESP32-CAM**: Object/sign recognition using AI vision (identify doors, signs, crossings)
- **MAX98357A Amplifier**: Louder, clearer audio guidance without headphones
- **Magnetometer/Compass**: Directional guidance ("turn North 40°")
- **Camera-based depth estimation**: Richer obstacle avoidance
- **Landmark recognition**: "Approach the blue door on your left"

---

## 📝 Notes

- The dashboard uses **browser geolocation** as a fallback when ESP32 is not connected
- Routes use the free **OSRM** routing engine (walking mode)
- Place search uses **OpenStreetMap Nominatim** (free, no API key)
- Nearby search uses **Overpass API** (free, no API key)
- Right-click anywhere on the map to set a custom destination for testing

---

*Built with ❤️ for the visually impaired community.*
