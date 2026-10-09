#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include "esp_camera.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#define HOSTNAME "smartcut-cam"
#define FLASH_LED_PIN 4

// AI-Thinker
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

WiFiManager wm;
bool portalMode = false;
WebServer server(80);
bool cameraOk = false;

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
  }
  return true;
}

void handleCapture() {
  if (!cameraOk) {
    server.send(503, "text/plain", "camera not ready");
    return;
  }
  bool led = server.hasArg("led") && server.arg("led") == "1";
  if (led) {
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(150);
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (led) digitalWrite(FLASH_LED_PIN, LOW);
  if (!fb) {
    server.send(500, "text/plain", "capture failed");
    return;
  }
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.client().write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  cameraOk = initCamera();
  Serial.println(cameraOk ? "camera ok" : "camera FAILED");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  if (strlen(WIFI_SSID)) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  } else {
    portalMode = true;
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(15);
    wm.autoConnect("SmartCut-Cam-Setup");
  }

  server.on("/capture", handleCapture);
  server.on("/", []() {
    server.send(200, "text/plain",
                String("SmartCut cam\ncamera: ") + (cameraOk ? "ok" : "fail") + "\nip: " + WiFi.localIP().toString() + "\n");
  });
  server.begin();
}

void loop() {
  if (portalMode) wm.process();
  static bool mdns = false;
  static uint32_t lastTry = 0, lastPrint = 0;
  if (WiFi.status() == WL_CONNECTED) {
    if (!mdns) {
      mdns = MDNS.begin(HOSTNAME);
      Serial.printf("wifi ok, ip %s, http://%s.local/capture\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    }
  } else {
    mdns = false;
    if (!portalMode && strlen(WIFI_SSID) && millis() - lastTry > 15000) {
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    if (millis() - lastPrint > 5000) {
      lastPrint = millis();
      Serial.println(strlen(WIFI_SSID) ? "wifi connecting..." : "wifi: connect to SmartCut-Cam-Setup");
    }
  }
  server.handleClient();
}
