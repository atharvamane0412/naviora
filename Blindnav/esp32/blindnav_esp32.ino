/*
 * ============================================================
 * BlindNav - ESP32 Firmware (Phase 1)
 * ============================================================
 * Hardware:
 *   - ESP32 DevKit
 *   - NEO-6M GPS Module (UART2: TX=17, RX=16)
 *   - HC-SR04 Ultrasonic Sensor (TRIG=5, ECHO=18)
 *   - Buzzer (GPIO 26)
 *
 * Features:
 *   - GPS data via NEO-6M + TinyGPS++
 *   - Obstacle detection via HC-SR04
 *   - WebSocket + HTTP POST to backend
 *   - Intelligent buzzer patterns
 *   - WiFi auto-reconnect
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>

// ── WiFi Credentials (change these) ──
const char* WIFI_SSID     = "Galaxy";
const char* WIFI_PASSWORD = "1223334444";

// ── Backend (change to your Render URL after deploying) ──
const char* WS_HOST = "blindnav-backend.onrender.com";
const int   WS_PORT    = 443;               // 443 for wss://, 80 for ws://
const char* WS_PATH    = "/esp32";
const char* HTTP_BASE  = "https://blindnav-backend.onrender.com";
bool        WS_SECURE  = true;              // true for wss://

// ── Pin Definitions ──
#define GPS_RX_PIN    16    // ESP32 RX2 ← GPS TX
#define GPS_TX_PIN    17    // ESP32 TX2 → GPS RX
#define GPS_BAUD      9600

#define TRIG_PIN      5
#define ECHO_PIN      18

#define BUZZER_PIN    26

// ── Thresholds ──
#define OBSTACLE_DIST_CM      80    // Alert if obstacle < 80cm
#define DANGER_DIST_CM        30    // Danger if < 30cm
#define GPS_SEND_INTERVAL_MS  2000  // Send GPS every 2 seconds
#define OBS_CHECK_INTERVAL_MS 200   // Check obstacle every 200ms
#define WS_SEND_INTERVAL_MS   3000  // WS heartbeat

// ── Buzzer Patterns ──
enum BuzzerPattern {
  BUZZER_OFF,
  BUZZER_GPS_SEARCHING,  // slow double beep
  BUZZER_GPS_FIXED,      // 3 rising beeps
  BUZZER_OBSTACLE_WARN,  // medium beep
  BUZZER_OBSTACLE_DANGER,// rapid beep
  BUZZER_WRONG_ROUTE,    // descending beep
  BUZZER_STARTUP,        // startup chime
};

// ── Global State ──
HardwareSerial gpsSerial(2);
TinyGPSPlus    gps;
WebSocketsClient wsClient;

volatile BuzzerPattern currentBuzzerPattern = BUZZER_STARTUP;
bool gpsFixed             = false;
bool wsConnected          = false;
float lastObstacleDist    = -1;
bool  obstacleDetected    = false;

unsigned long lastGPSSend      = 0;
unsigned long lastObsCheck     = 0;
unsigned long lastWSSend       = 0;
unsigned long lastWifiCheck    = 0;
unsigned long buzzerLastToggle = 0;
int buzzerBeepCount            = 0;
bool buzzerState               = false;

// ── Function Prototypes ──
void connectWifi();
void connectWebSocket();
void sendGPSData();
void sendObstacleData(bool detected, float distance);
float measureDistance();
void handleBuzzer();
void setBuzzerPattern(BuzzerPattern p);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
String buildGPSJson();

// ── Setup ──
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n╔═══════════════════════╗");
  Serial.println("║   BlindNav ESP32 v1   ║");
  Serial.println("╚═══════════════════════╝");

  // Pin modes
  pinMode(TRIG_PIN,  OUTPUT);
  pinMode(ECHO_PIN,  INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Startup chime
  setBuzzerPattern(BUZZER_STARTUP);
  delay(800);

  // Init GPS serial
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] Serial started on UART2");

  // Connect WiFi
  connectWifi();

  // Connect WebSocket
  connectWebSocket();

  // Begin GPS searching pattern
  setBuzzerPattern(BUZZER_GPS_SEARCHING);
  Serial.println("[INIT] Setup complete");
}

// ── Main Loop ──
void loop() {
  unsigned long now = millis();

  // Feed GPS parser
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // Check WiFi
  if (now - lastWifiCheck > 10000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Disconnected. Reconnecting…");
      connectWifi();
    }
  }

  // WebSocket loop
  wsClient.loop();

  // Obstacle check
  if (now - lastObsCheck > OBS_CHECK_INTERVAL_MS) {
    lastObsCheck = now;
    float dist = measureDistance();
    bool newObstacle = (dist > 0 && dist < OBSTACLE_DIST_CM);

    if (newObstacle != obstacleDetected || abs(dist - lastObstacleDist) > 5) {
      obstacleDetected = newObstacle;
      lastObstacleDist = dist;

      if (obstacleDetected) {
        if (dist < DANGER_DIST_CM) {
          setBuzzerPattern(BUZZER_OBSTACLE_DANGER);
        } else {
          setBuzzerPattern(BUZZER_OBSTACLE_WARN);
        }
        sendObstacleData(true, dist);
        Serial.printf("[OBSTACLE] Detected at %.1f cm\n", dist);
      } else {
        if (gpsFixed) setBuzzerPattern(BUZZER_OFF);
        else          setBuzzerPattern(BUZZER_GPS_SEARCHING);
        sendObstacleData(false, dist);
      }
    }
  }

  // GPS data send
  if (now - lastGPSSend > GPS_SEND_INTERVAL_MS) {
    lastGPSSend = now;

    if (gps.location.isUpdated() && gps.location.isValid()) {
      if (!gpsFixed) {
        gpsFixed = true;
        setBuzzerPattern(BUZZER_GPS_FIXED);
        Serial.println("[GPS] Fixed! Satellites: " + String(gps.satellites.value()));
        delay(600);
        if (!obstacleDetected) setBuzzerPattern(BUZZER_OFF);
      }
      sendGPSData();
    } else {
      Serial.printf("[GPS] Chars: %lu, Sentences: %lu, Failures: %lu\n",
        gps.charsProcessed(), gps.sentencesWithFix(), gps.failedChecksum());
    }
  }

  // Handle buzzer pattern
  handleBuzzer();
}

// ── WiFi Connection ──
void connectWifi() {
  Serial.printf("[WiFi] Connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed to connect. Will retry.");
  }
}

// ── WebSocket ──
void connectWebSocket() {
  Serial.printf("[WS] Connecting to %s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);

  if (WS_SECURE) {
    wsClient.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  } else {
    wsClient.begin(WS_HOST, WS_PORT, WS_PATH);
  }

  wsClient.onEvent(webSocketEvent);
  wsClient.setReconnectInterval(5000);
  wsClient.enableHeartbeat(15000, 3000, 2);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      Serial.println("[WS] Connected to backend");
      // Send hello
      {
        StaticJsonDocument<128> doc;
        doc["type"] = "status";
        JsonObject data = doc.createNestedObject("data");
        data["device"] = "ESP32-BlindNav";
        data["version"] = "1.0";
        data["gpsConnected"] = gpsFixed;
        String out; serializeJson(doc, out);
        wsClient.sendTXT(out);
      }
      break;

    case WStype_DISCONNECTED:
      wsConnected = false;
      Serial.println("[WS] Disconnected");
      break;

    case WStype_TEXT:
      Serial.printf("[WS] Received: %s\n", payload);
      // Handle commands from dashboard (e.g., buzzer commands)
      {
        StaticJsonDocument<256> doc;
        if (!deserializeJson(doc, payload)) {
          const char* cmd = doc["command"];
          if (cmd && strcmp(cmd, "buzz_wrong_route") == 0) {
            setBuzzerPattern(BUZZER_WRONG_ROUTE);
          }
        }
      }
      break;

    case WStype_ERROR:
      Serial.println("[WS] Error");
      break;

    default:
      break;
  }
}

// ── GPS Data Send ──
String buildGPSJson() {
  StaticJsonDocument<256> doc;
  doc["type"] = "gps";
  JsonObject data = doc.createNestedObject("data");
  data["lat"]       = gps.location.lat();
  data["lng"]       = gps.location.lng();
  data["speed"]     = gps.speed.isValid() ? gps.speed.kmph() : 0;
  data["course"]    = gps.course.isValid() ? gps.course.deg() : 0;
  data["satellites"]= gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  data["hdop"]      = gps.hdop.isValid() ? gps.hdop.hdop() : 99;
  data["altitude"]  = gps.altitude.isValid() ? gps.altitude.meters() : 0;
  String output; serializeJson(doc, output);
  return output;
}

void sendGPSData() {
  float lat = gps.location.lat();
  float lng = gps.location.lng();

  Serial.printf("[GPS] Lat: %.6f, Lng: %.6f, Sats: %d, Speed: %.1f km/h\n",
    lat, lng, (int)gps.satellites.value(), gps.speed.kmph());

  // Try WebSocket first
  if (wsConnected) {
    String json = buildGPSJson();
    wsClient.sendTXT(json);
  } else if (WiFi.status() == WL_CONNECTED) {
    // HTTP fallback
    HTTPClient http;
    String url = String(HTTP_BASE) + "/api/gps";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["lat"]        = lat;
    doc["lng"]        = lng;
    doc["speed"]      = gps.speed.isValid() ? gps.speed.kmph() : 0;
    doc["satellites"] = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    doc["hdop"]       = gps.hdop.isValid() ? gps.hdop.hdop() : 99;
    String body; serializeJson(doc, body);

    int code = http.POST(body);
    Serial.printf("[HTTP] GPS sent, response: %d\n", code);
    http.end();
  }
}

void sendObstacleData(bool detected, float distance) {
  if (!wsConnected && WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<128> doc;
  doc["type"]     = "obstacle";
  doc["detected"] = detected;
  doc["distance"] = distance;
  String json; serializeJson(doc, json);

  if (wsConnected) {
    wsClient.sendTXT(json);
  } else {
    HTTPClient http;
    http.begin(String(HTTP_BASE) + "/api/obstacle");
    http.addHeader("Content-Type", "application/json");
    http.POST(json);
    http.end();
  }
}

// ── Ultrasonic Sensor ──
float measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  if (duration == 0) return -1; // No echo

  float distance = (duration * 0.0343) / 2.0; // cm
  return distance;
}

// ── Buzzer Pattern Handler (non-blocking) ──
void setBuzzerPattern(BuzzerPattern p) {
  currentBuzzerPattern = p;
  buzzerBeepCount = 0;
  buzzerLastToggle = 0;
  buzzerState = false;
  digitalWrite(BUZZER_PIN, LOW);
}

void handleBuzzer() {
  unsigned long now = millis();

  switch (currentBuzzerPattern) {
    case BUZZER_OFF:
      digitalWrite(BUZZER_PIN, LOW);
      break;

    case BUZZER_STARTUP: {
      // 3 ascending tones (simulated with on/off durations)
      static int startupStep = 0;
      if (now - buzzerLastToggle > (200 - startupStep * 40)) {
        buzzerLastToggle = now;
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState);
        if (!buzzerState) startupStep++;
        if (startupStep >= 3) {
          currentBuzzerPattern = BUZZER_OFF;
          startupStep = 0;
        }
      }
      break;
    }

    case BUZZER_GPS_SEARCHING: {
      // Double beep every 2 seconds: beep-beep ... pause ...
      static int phase = 0;
      static const int pattern[] = {100, 100, 100, 1600}; // on, off, on, pause
      if (now - buzzerLastToggle > pattern[phase]) {
        buzzerLastToggle = now;
        phase = (phase + 1) % 4;
        digitalWrite(BUZZER_PIN, (phase == 0 || phase == 2) ? HIGH : LOW);
      }
      break;
    }

    case BUZZER_GPS_FIXED: {
      // 3 quick rising beeps
      if (now - buzzerLastToggle > 120) {
        buzzerLastToggle = now;
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState);
        if (!buzzerState) buzzerBeepCount++;
        if (buzzerBeepCount >= 3) {
          currentBuzzerPattern = BUZZER_OFF;
          buzzerBeepCount = 0;
        }
      }
      break;
    }

    case BUZZER_OBSTACLE_WARN: {
      // Medium beep every 600ms
      if (now - buzzerLastToggle > (buzzerState ? 200 : 400)) {
        buzzerLastToggle = now;
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState);
      }
      break;
    }

    case BUZZER_OBSTACLE_DANGER: {
      // Rapid beep every 150ms
      if (now - buzzerLastToggle > 100) {
        buzzerLastToggle = now;
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState);
      }
      break;
    }

    case BUZZER_WRONG_ROUTE: {
      // Long low beep x2 (descending)
      if (now - buzzerLastToggle > (buzzerState ? 400 : 200)) {
        buzzerLastToggle = now;
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState);
        if (!buzzerState) buzzerBeepCount++;
        if (buzzerBeepCount >= 2) {
          currentBuzzerPattern = gpsFixed ? BUZZER_OFF : BUZZER_GPS_SEARCHING;
          buzzerBeepCount = 0;
        }
      }
      break;
    }
  }
}
