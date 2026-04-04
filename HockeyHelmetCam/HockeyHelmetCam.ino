/*
 * HockeyHelmetCam — ESP32-S3 Hockey Helmet Camera Firmware
 * Board: Freenove ESP32-S3 WROOM (CAMERA_MODEL_ESP32S3_EYE)
 * 
 * Features:
 *   - Wi-Fi AP mode: creates "HelmetCam" hotspot, no router needed
 *   - MJPEG live stream: view on phone browser at http://192.168.4.1
 *   - BLE remote control: start/stop recording, take photo, change settings
 *   - SD card: saves photos and recording frames to microSD
 *   - WS2812 LED: color-coded status indicator
 *   - Boot button (GPIO0): quick photo capture
 * 
 * LED status colors:
 *   RED    = error (camera or SD failed)
 *   GREEN  = ready / standby
 *   BLUE   = Wi-Fi streaming active
 *   PURPLE = recording to SD card
 *   WHITE  = photo captured (flash)
 *   OFF    = idle / deep sleep
 *
 * Arduino IDE settings:
 *   Board:            ESP32S3 Dev Module
 *   PSRAM:            OPI PSRAM
 *   Partition Scheme: 8M with spiffs (3MB APP/1.5MB SPIFFS)
 *   Flash Size:       8MB (64Mb)
 *   CPU Frequency:    240MHz (WiFi)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEUtils.h"
#include "BLE2902.h"
#include "FS.h"
#include "SD_MMC.h"
#include "Freenove_WS2812_Lib_for_ESP32.h"

// ─── PIN DEFINITIONS ──────────────────────────────────────────────
// Camera (hardwired on Freenove board — do NOT change)
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

// SD card (hardwired on Freenove board — do NOT change)
#define SD_MMC_CMD  38
#define SD_MMC_CLK  39
#define SD_MMC_D0   40

// WS2812 RGB LED
#define WS2812_PIN  48

// Boot button for quick photo
#define BUTTON_PIN   0

// ─── WI-FI AP SETTINGS ───────────────────────────────────────────
const char* ap_ssid     = "HelmetCam";
const char* ap_password = "hockey123";  // min 8 chars, change this!

// ─── BLE SETTINGS ─────────────────────────────────────────────────
#define BLE_NAME "HelmetCam"
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ─── GLOBALS ──────────────────────────────────────────────────────
WebServer server(80);
Freenove_ESP32_WS2812 led(1, WS2812_PIN, 1, TYPE_GRB);

BLECharacteristic* pTxCharacteristic;
bool bleConnected   = false;
bool isRecording    = false;
bool isStreaming     = false;
bool sdReady        = false;
bool cameraReady    = false;
int  photoCount     = 0;
int  recordingFrame = 0;
int  recordingSession = 0;
unsigned long lastFrameTime = 0;
unsigned long recordStartTime = 0;

// Recording frame interval in ms (controls FPS saved to SD)
// 100ms = ~10 FPS, 200ms = ~5 FPS (lower = more storage used)
#define RECORD_INTERVAL_MS  200

// ─── LED STATUS ───────────────────────────────────────────────────
enum LedColor { LED_OFF, LED_RED, LED_GREEN, LED_BLUE, LED_PURPLE, LED_WHITE, LED_YELLOW };

void setLed(LedColor color) {
  switch (color) {
    case LED_OFF:    led.setLedColorData(0, 0, 0, 0);       break;
    case LED_RED:    led.setLedColorData(0, 255, 0, 0);     break;
    case LED_GREEN:  led.setLedColorData(0, 0, 255, 0);     break;
    case LED_BLUE:   led.setLedColorData(0, 0, 0, 255);     break;
    case LED_PURPLE: led.setLedColorData(0, 180, 0, 255);   break;
    case LED_WHITE:  led.setLedColorData(0, 255, 255, 255); break;
    case LED_YELLOW: led.setLedColorData(0, 255, 255, 0);   break;
  }
  led.show();
}

void flashLed(LedColor color, int ms) {
  setLed(color);
  delay(ms);
  updateStatusLed();
}

void updateStatusLed() {
  if (!cameraReady)    { setLed(LED_RED);    return; }
  if (isRecording)     { setLed(LED_PURPLE); return; }
  if (isStreaming)     { setLed(LED_BLUE);   return; }
  setLed(LED_GREEN);
}

// ─── SD CARD ──────────────────────────────────────────────────────
void initSD() {
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.println("[SD] Mount failed");
    sdReady = false;
    return;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] No card inserted");
    sdReady = false;
    return;
  }
  
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  uint64_t usedSpace = SD_MMC.usedBytes() / (1024 * 1024);
  Serial.printf("[SD] Card: %lluMB total, %lluMB used\n", cardSize, usedSpace);
  
  // Create directories
  if (!SD_MMC.exists("/photos"))  SD_MMC.mkdir("/photos");
  if (!SD_MMC.exists("/videos"))  SD_MMC.mkdir("/videos");
  
  // Count existing photos to resume numbering
  File dir = SD_MMC.open("/photos");
  if (dir) {
    File f = dir.openNextFile();
    while (f) { photoCount++; f = dir.openNextFile(); }
  }
  
  // Count existing recording sessions
  dir = SD_MMC.open("/videos");
  if (dir) {
    File f = dir.openNextFile();
    while (f) {
      if (f.isDirectory()) recordingSession++;
      f = dir.openNextFile();
    }
  }
  
  sdReady = true;
  Serial.printf("[SD] Ready. %d existing photos, %d recording sessions\n", photoCount, recordingSession);
}

bool savePhoto() {
  if (!sdReady || !cameraReady) return false;
  
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Photo] Capture failed");
    return false;
  }
  
  String path = "/photos/IMG_" + String(photoCount) + ".jpg";
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("[Photo] File open failed");
    esp_camera_fb_return(fb);
    return false;
  }
  
  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);
  
  Serial.printf("[Photo] Saved %s (%d bytes)\n", path.c_str(), fb->len);
  photoCount++;
  return true;
}

bool saveRecordingFrame() {
  if (!sdReady || !cameraReady || !isRecording) return false;
  
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return false;
  
  String path = "/videos/REC_" + String(recordingSession) 
              + "/frame_" + String(recordingFrame) + ".jpg";
  File file = SD_MMC.open(path, FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    recordingFrame++;
  }
  
  esp_camera_fb_return(fb);
  return true;
}

void startRecording() {
  if (!sdReady || isRecording) return;
  
  recordingSession++;
  String dir = "/videos/REC_" + String(recordingSession);
  SD_MMC.mkdir(dir.c_str());
  
  recordingFrame = 0;
  recordStartTime = millis();
  isRecording = true;
  
  Serial.printf("[Record] Started session %d\n", recordingSession);
  bleSend("REC:START");
  updateStatusLed();
}

void stopRecording() {
  if (!isRecording) return;
  
  unsigned long duration = (millis() - recordStartTime) / 1000;
  isRecording = false;
  
  Serial.printf("[Record] Stopped. %d frames, %lu seconds\n", recordingFrame, duration);
  bleSend("REC:STOP:" + String(recordingFrame) + "frames," + String(duration) + "s");
  updateStatusLed();
}

// ─── CAMERA ───────────────────────────────────────────────────────
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;       // 0-63, lower = better quality
  config.fb_count     = 2;
  
  // Default resolution: SVGA (800x600) — good balance of quality and speed
  // Change to FRAMESIZE_VGA (640x480) for faster streaming
  // or FRAMESIZE_XGA (1024x768) for better stills
  config.frame_size = FRAMESIZE_SVGA;
  
  if (!psramFound()) {
    Serial.println("[Camera] WARNING: No PSRAM! Limiting to VGA");
    config.frame_size   = FRAMESIZE_VGA;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.fb_count     = 1;
    config.jpeg_quality = 12;
  }
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init failed: 0x%x\n", err);
    return false;
  }
  
  // Tune the image sensor
  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);
  s->set_brightness(s, 2);         // max brightness
  s->set_saturation(s, 0);
  s->set_contrast(s, 1);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);           // auto white balance
  s->set_exposure_ctrl(s, 1);     // auto exposure ON
  s->set_aec2(s, 1);              // auto exposure DSP ON
  s->set_ae_level(s, 2);          // exposure compensation +2 (brighter)
  s->set_aec_value(s, 600);       // manual exposure base value (higher = brighter)
  s->set_gain_ctrl(s, 1);         // auto gain ON
  s->set_agc_gain(s, 15);         // starting gain boost (0-30)
  s->set_gainceiling(s, (gainceiling_t)6);  // max gain ceiling (0-6, 6 = 128x)
  
  Serial.println("[Camera] Initialized OK");
  return true;
}

void setResolution(framesize_t size) {
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, size);
  Serial.printf("[Camera] Resolution changed to %d\n", size);
}

// ─── WI-FI AP + MJPEG STREAM ─────────────────────────────────────
// MJPEG stream boundary
#define BOUNDARY "helmetcam_frame"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" BOUNDARY
#define FRAME_HEADER "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: "

WiFiClient streamClient;

void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>HelmetCam</title>
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body { background:#111; color:#fff; font-family:system-ui; text-align:center; }
    h1 { padding:12px; font-size:20px; font-weight:500; }
    img { width:100%; max-width:800px; border-radius:8px; }
    .controls { display:flex; flex-wrap:wrap; justify-content:center; gap:8px; padding:12px; }
    button {
      padding:10px 20px; border:none; border-radius:6px;
      font-size:14px; font-weight:500; cursor:pointer;
      color:#fff; background:#333; transition:background 0.2s;
    }
    button:active { background:#555; }
    .rec { background:#c0392b; }
    .rec.active { background:#e74c3c; animation:pulse 1s infinite; }
    .photo { background:#2980b9; }
    .info { padding:8px; font-size:12px; color:#888; }
    @keyframes pulse { 50% { opacity:0.7; } }
  </style>
</head>
<body>
  <h1>HelmetCam</h1>
  <img id="stream" src="/stream">
  <div class="controls">
    <button class="rec" id="recBtn" onclick="toggleRecord()">Record</button>
    <button class="photo" onclick="takePhoto()">Photo</button>
    <button onclick="setRes('vga')">VGA</button>
    <button onclick="setRes('svga')">SVGA</button>
    <button onclick="setRes('xga')">XGA</button>
  </div>
  <div class="info" id="status">Connected</div>
  <script>
    let recording = false;
    function toggleRecord() {
      fetch(recording ? '/stop' : '/record');
      recording = !recording;
      const btn = document.getElementById('recBtn');
      btn.textContent = recording ? 'Stop' : 'Record';
      btn.classList.toggle('active', recording);
      document.getElementById('status').textContent = recording ? 'Recording...' : 'Standby';
    }
    function takePhoto() {
      fetch('/photo').then(r => r.text()).then(t => {
        document.getElementById('status').textContent = t;
      });
    }
    function setRes(r) {
      fetch('/resolution?size=' + r).then(res => res.text()).then(t => {
        document.getElementById('status').textContent = t;
        document.getElementById('stream').src = '/stream?' + Date.now();
      });
    }
  </script>
</body>
</html>
  )rawhtml";
  server.send(200, "text/html", html);
}

void handleStream() {
  WiFiClient client = server.client();
  
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: " STREAM_CONTENT_TYPE "\r\n";
  response += "Access-Control-Allow-Origin: *\r\n\r\n";
  client.print(response);
  
  isStreaming = true;
  updateStatusLed();
  
  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[Stream] Frame capture failed");
      break;
    }
    
    String header = FRAME_HEADER + String(fb->len) + "\r\n\r\n";
    client.print(header);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    
    esp_camera_fb_return(fb);
    
    if (!client.connected()) break;
  }
  
  isStreaming = false;
  updateStatusLed();
  Serial.println("[Stream] Client disconnected");
}

void handlePhoto() {
  if (savePhoto()) {
    flashLed(LED_WHITE, 150);
    server.send(200, "text/plain", "Photo saved #" + String(photoCount - 1));
  } else {
    server.send(500, "text/plain", "Photo failed");
  }
}

void handleRecord() {
  startRecording();
  server.send(200, "text/plain", "Recording started");
}

void handleStop() {
  stopRecording();
  server.send(200, "text/plain", "Recording stopped");
}

void handleResolution() {
  String size = server.arg("size");
  if      (size == "vga")  setResolution(FRAMESIZE_VGA);
  else if (size == "svga") setResolution(FRAMESIZE_SVGA);
  else if (size == "xga")  setResolution(FRAMESIZE_XGA);
  else if (size == "hd")   setResolution(FRAMESIZE_HD);
  else if (size == "uxga") setResolution(FRAMESIZE_UXGA);
  else {
    server.send(400, "text/plain", "Unknown: " + size);
    return;
  }
  server.send(200, "text/plain", "Resolution: " + size);
}

void handleStatus() {
  String json = "{";
  json += "\"camera\":" + String(cameraReady ? "true" : "false") + ",";
  json += "\"sd\":" + String(sdReady ? "true" : "false") + ",";
  json += "\"recording\":" + String(isRecording ? "true" : "false") + ",";
  json += "\"streaming\":" + String(isStreaming ? "true" : "false") + ",";
  json += "\"photos\":" + String(photoCount) + ",";
  json += "\"sessions\":" + String(recordingSession);
  if (sdReady) {
    json += ",\"sd_total_mb\":" + String((uint32_t)(SD_MMC.totalBytes() / (1024 * 1024)));
    json += ",\"sd_used_mb\":" + String((uint32_t)(SD_MMC.usedBytes() / (1024 * 1024)));
  }
  json += "}";
  server.send(200, "application/json", json);
}

void initWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  delay(100);
  
  Serial.printf("[WiFi] AP started: %s\n", ap_ssid);
  Serial.printf("[WiFi] IP: %s\n", WiFi.softAPIP().toString().c_str());
  
  // Register HTTP routes
  server.on("/",           HTTP_GET, handleRoot);
  server.on("/stream",     HTTP_GET, handleStream);
  server.on("/photo",      HTTP_GET, handlePhoto);
  server.on("/record",     HTTP_GET, handleRecord);
  server.on("/stop",       HTTP_GET, handleStop);
  server.on("/resolution", HTTP_GET, handleResolution);
  server.on("/status",     HTTP_GET, handleStatus);
  server.begin();
  
  Serial.println("[WiFi] Web server started");
}

// ─── BLE CONTROL ──────────────────────────────────────────────────
void bleSend(String msg) {
  if (bleConnected && pTxCharacteristic) {
    pTxCharacteristic->setValue(msg.c_str());
    pTxCharacteristic->notify();
  }
}

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    Serial.println("[BLE] Client connected");
    bleSend("HELLO:HelmetCam");
  }
  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    Serial.println("[BLE] Client disconnected");
    // Restart advertising so phone can reconnect
    pServer->getAdvertising()->start();
  }
};

class BleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    String cmd = pChar->getValue();
    cmd.trim();
    cmd.toUpperCase();
    
    Serial.printf("[BLE] Command: %s\n", cmd.c_str());
    
    if (cmd == "PHOTO") {
      if (savePhoto()) {
        flashLed(LED_WHITE, 150);
        bleSend("OK:PHOTO:" + String(photoCount - 1));
      } else {
        bleSend("ERR:PHOTO");
      }
    }
    else if (cmd == "RECORD" || cmd == "REC") {
      startRecording();
    }
    else if (cmd == "STOP") {
      stopRecording();
    }
    else if (cmd == "STATUS") {
      String s = "STATUS:cam=" + String(cameraReady) 
               + ",sd=" + String(sdReady)
               + ",rec=" + String(isRecording)
               + ",photos=" + String(photoCount);
      bleSend(s);
    }
    else if (cmd == "RES:VGA")  { setResolution(FRAMESIZE_VGA);  bleSend("OK:VGA");  }
    else if (cmd == "RES:SVGA") { setResolution(FRAMESIZE_SVGA); bleSend("OK:SVGA"); }
    else if (cmd == "RES:XGA")  { setResolution(FRAMESIZE_XGA);  bleSend("OK:XGA");  }
    else if (cmd == "RES:HD")   { setResolution(FRAMESIZE_HD);   bleSend("OK:HD");   }
    else if (cmd == "FLIP") {
      sensor_t* s = esp_camera_sensor_get();
      int v = s->status.vflip;
      s->set_vflip(s, !v);
      bleSend("OK:FLIP:" + String(!v));
    }
    else if (cmd == "MIRROR") {
      sensor_t* s = esp_camera_sensor_get();
      int m = s->status.hmirror;
      s->set_hmirror(s, !m);
      bleSend("OK:MIRROR:" + String(!m));
    }
    else {
      bleSend("ERR:UNKNOWN:" + cmd);
    }
  }
};

void initBLE() {
  BLEDevice::init(BLE_NAME);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BleServerCallbacks());
  
  BLEService* pService = pServer->createService(SERVICE_UUID);
  
  // TX characteristic (helmet → phone)
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());
  
  // RX characteristic (phone → helmet)
  BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE
  );
  pRxCharacteristic->setCallbacks(new BleRxCallbacks());
  
  pService->start();
  pServer->getAdvertising()->start();
  Serial.println("[BLE] Advertising as: " BLE_NAME);
}

// ─── BUTTON HANDLING ──────────────────────────────────────────────
unsigned long lastButtonPress = 0;
bool buttonWasPressed = false;

void checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!buttonWasPressed && (millis() - lastButtonPress > 300)) {
      buttonWasPressed = true;
      lastButtonPress = millis();
    }
  } else {
    if (buttonWasPressed) {
      unsigned long held = millis() - lastButtonPress;
      buttonWasPressed = false;
      
      if (held > 2000) {
        // Long press (2+ sec): toggle recording
        if (isRecording) stopRecording();
        else startRecording();
      } else {
        // Short press: take photo
        if (savePhoto()) {
          flashLed(LED_WHITE, 150);
          bleSend("OK:PHOTO:" + String(photoCount - 1));
        }
      }
    }
  }
}

// ─── SETUP ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== HockeyHelmetCam ===");
  
  // Init LED first so we can show status
  led.begin();
  led.setBrightness(20);
  setLed(LED_YELLOW);  // booting
  
  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Camera
  Serial.println("[Boot] Initializing camera...");
  cameraReady = initCamera();
  if (!cameraReady) {
    Serial.println("[Boot] CAMERA FAILED!");
    setLed(LED_RED);
    // Don't return — still start Wi-Fi/BLE so user can debug
  }
  
  // SD card
  Serial.println("[Boot] Initializing SD card...");
  initSD();
  if (!sdReady) {
    Serial.println("[Boot] SD card not available — recording disabled");
  }
  
  // Wi-Fi AP
  Serial.println("[Boot] Starting Wi-Fi AP...");
  initWiFiAP();
  
  // BLE
  Serial.println("[Boot] Starting BLE...");
  initBLE();
  
  // Ready
  updateStatusLed();
  Serial.println("\n=== READY ===");
  Serial.printf("Wi-Fi: Connect to '%s' (password: %s)\n", ap_ssid, ap_password);
  Serial.printf("Stream: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("BLE: Scan for '%s'\n", BLE_NAME);
  Serial.printf("Button: Short press = photo, Long press (2s) = record\n");
  Serial.println("============================\n");
}

// ─── MAIN LOOP ────────────────────────────────────────────────────
void loop() {
  // Handle web requests
  server.handleClient();
  
  // Handle button
  checkButton();
  
  // Handle recording (save frames at interval)
  if (isRecording && (millis() - lastFrameTime >= RECORD_INTERVAL_MS)) {
    lastFrameTime = millis();
    saveRecordingFrame();
    
    // Blink LED while recording
    if ((recordingFrame % 10) < 5) setLed(LED_PURPLE);
    else setLed(LED_OFF);
  }
  
  // Small delay to prevent watchdog issues
  delay(1);
}
