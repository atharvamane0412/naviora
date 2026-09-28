/*
 * ============================================================
 * BlindNav - ESP32-CAM Firmware (Phase 2)
 * ============================================================
 * Hardware: AI-Thinker ESP32-CAM module
 *
 * Behaviour:
 *   - Connects to WiFi + WebSocket backend (separate connection
 *     from the main ESP32 navigation board)
 *   - Stays IDLE most of the time (camera not streaming)
 *   - When it receives a "capture" command from the backend
 *     (triggered by the dashboard's "What's ahead" voice command),
 *     it grabs ONE JPEG frame and sends it back over WebSocket
 *   - This keeps WiFi bandwidth & power usage minimal — no
 *     continuous video streaming, matching the on-demand design.
 *
 * Wiring: Standard AI-Thinker ESP32-CAM pin map (no extra wiring
 * needed beyond power + the FTDI/USB-TTL programmer).
 * ============================================================
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "esp_camera.h"

// ── WiFi Credentials (match your main ESP32 board) ──
const char* WIFI_SSID     = "Galaxy";
const char* WIFI_PASSWORD = "1223334444";

// ── Backend ──
const char* WS_HOST   = "blindnav-backend.onrender.com";
const int   WS_PORT   = 443;
const char* WS_PATH   = "/esp32cam";   // separate path from /esp32
bool        WS_SECURE = true;

// ── AI-Thinker ESP32-CAM Pin Map ──
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_LED_PIN      4   // onboard flash LED, used as a "capturing" indicator

WebSocketsClient wsClient;
bool wsConnected = false;
bool cameraReady = false;

void connectWifi() {
  Serial.printf("[WiFi] Connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed to connect. Will retry.");
  }
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;

  // Frame size kept moderate — small enough to send quickly over
  // WiFi, large enough for YOLO to detect objects reasonably well.
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;   // 640x480
    config.jpeg_quality  = 12;             // lower = higher quality
    config.fb_count      = 2;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;  // fallback, still fine
    config.jpeg_quality  = 14;
    config.fb_count      = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init failed: 0x%x\n", err);
    return false;
  }
  Serial.println("[CAM] Camera initialised");
  return true;
}

// ── Capture one frame and send it over WebSocket as binary ──
void captureAndSend() {
  if (!cameraReady) {
    Serial.println("[CAM] Not ready, skipping capture");
    return;
  }

  digitalWrite(FLASH_LED_PIN, HIGH); // brief flash to indicate capture
  camera_fb_t* fb = esp_camera_fb_get();
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!fb) {
    Serial.println("[CAM] Capture failed");
    sendError("capture_failed");
    return;
  }

  Serial.printf("[CAM] Captured frame: %u bytes\n", fb->len);

  if (wsConnected) {
    // Send a small JSON header first so backend/dashboard knows a
    // binary frame is coming next, then send the raw JPEG bytes.
    StaticJsonDocument<128> doc;
    doc["type"] = "frame_meta";
    doc["size"] = fb->len;
    doc["width"] = fb->width;
    doc["height"] = fb->height;
    String meta;
    serializeJson(doc, meta);
    wsClient.sendTXT(meta);

    wsClient.sendBIN(fb->buf, fb->len);
    Serial.println("[CAM] Frame sent over WebSocket");
  } else {
    Serial.println("[CAM] WebSocket not connected, frame dropped");
  }

  esp_camera_fb_return(fb);
}

void sendError(const char* reason) {
  if (!wsConnected) return;
  StaticJsonDocument<128> doc;
  doc["type"] = "error";
  doc["reason"] = reason;
  String out; serializeJson(doc, out);
  wsClient.sendTXT(out);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      Serial.println("[WS] Connected to backend (/esp32cam)");
      {
        StaticJsonDocument<128> doc;
        doc["type"] = "status";
        JsonObject data = doc.createNestedObject("data");
        data["device"] = "ESP32-CAM-BlindNav";
        data["ready"] = cameraReady;
        String out; serializeJson(doc, out);
        wsClient.sendTXT(out);
      }
      break;

    case WStype_DISCONNECTED:
      wsConnected = false;
      Serial.println("[WS] Disconnected");
      break;

    case WStype_TEXT: {
      Serial.printf("[WS] Received: %s\n", payload);
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, payload)) {
        const char* cmd = doc["command"];
        if (cmd && strcmp(cmd, "capture") == 0) {
          Serial.println("[CAM] Capture command received");
          captureAndSend();
        }
      }
      break;
    }

    default:
      break;
  }
}

void connectWebSocket() {
  Serial.printf("[WS] Connecting to %s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);
  if (WS_SECURE) wsClient.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  else           wsClient.begin(WS_HOST, WS_PORT, WS_PATH);
  wsClient.onEvent(webSocketEvent);
  wsClient.setReconnectInterval(5000);
  wsClient.enableHeartbeat(15000, 3000, 2);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n╔═══════════════════════════╗");
  Serial.println("║  BlindNav ESP32-CAM v1    ║");
  Serial.println("╚═══════════════════════════╝");

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  cameraReady = initCamera();

  connectWifi();
  connectWebSocket();

  Serial.println("[INIT] Ready — waiting for capture commands");
}

void loop() {
  wsClient.loop();

  // WiFi watchdog
  static unsigned long lastWifiCheck = 0;
  unsigned long now = millis();
  if (now - lastWifiCheck > 10000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Disconnected. Reconnecting…");
      connectWifi();
    }
  }
}
