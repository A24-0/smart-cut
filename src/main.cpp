#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#include "font_ru.h"

TFT_eSPI tft;

// ---------------- Настройки цикла нарезки ----------------
#define CONVEYOR_LOOP_MS 5000   // время одного полного круга конвейера, мс
#define CONVEYOR_SPEED 165      // скорость конвейера 0..255 (меньше = медленнее)
#define SCREW_STEPS 90000       // шагов ШВП от верха до низа (ход 30 см)
#define SCREW_DOWN_DIR -1       // 1 или -1: если ШВП едет не туда, поменять знак
#define STEP_DELAY_US 400       // пауза между шагами ШВП: меньше = быстрее

// ---------------- Пины ----------------
// Конвейер: L298N
#define ENA 15
#define IN1 2
#define IN2 4
#define IN3 16
#define IN4 17
#define ENB 5
// ШВП: NEMA 17 (A+, A-, B+, B-)
#define ST1 13
#define ST2 12
#define ST3 14
#define ST4 27
// Тач XPT2046 (программный SPI)
#define T_CLK 19
#define T_CS 21
#define T_DIN 22
#define T_DO 23
#define TOUCH_Z_MIN 100

const uint8_t stepSeq[4][4] = {
  {1, 0, 1, 0},
  {0, 1, 1, 0},
  {0, 1, 0, 1},
  {1, 0, 0, 1},
};

// ---------------- Состояние цикла ----------------
enum Stage { IDLE, WAIT_CONFIRM, CONVEYOR, SCREW_DOWN, SCREW_UP, DONE };
Stage stage = IDLE;
uint32_t stageStart = 0;
long stepsLeft = 0;
int screwDir = 1;

String stepProduct, stepCut;
int stepGrams = -1;
bool stepAuto = false;

// ---------------- MQTT ----------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String topicCmd, topicStatus;
uint32_t lastStatus = 0, lastMqttTry = 0;

const char* stateName() {
  switch (stage) {
    case IDLE: return "idle";
    case WAIT_CONFIRM: return "waiting_confirm";
    case DONE: return "done";
    default: return "running";
  }
}

void publishStatus() {
  if (!mqtt.connected()) return;
  JsonDocument d;
  d["state"] = stateName();
  d["stage"] = (int)stage;
  char buf[96];
  serializeJson(d, buf);
  mqtt.publish(topicStatus.c_str(), buf);
  lastStatus = millis();
}

// ---------------- Моторы ----------------
void conveyorOn() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  ledcWrite(0, CONVEYOR_SPEED);
  ledcWrite(1, CONVEYOR_SPEED);
}

void conveyorOff() {
  ledcWrite(0, 0);
  ledcWrite(1, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void releaseScrew() {
  digitalWrite(ST1, LOW);
  digitalWrite(ST2, LOW);
  digitalWrite(ST3, LOW);
  digitalWrite(ST4, LOW);
}

void stepMotor() {
  static uint8_t idx = 0;
  static uint32_t last = 0;
  if (stepsLeft <= 0 || micros() - last < STEP_DELAY_US) return;
  last = micros();
  digitalWrite(ST1, stepSeq[idx][0]);
  digitalWrite(ST2, stepSeq[idx][1]);
  digitalWrite(ST3, stepSeq[idx][2]);
  digitalWrite(ST4, stepSeq[idx][3]);
  idx = (idx + (screwDir > 0 ? 1 : 3)) % 4;
  stepsLeft--;
}

// ---------------- Тач ----------------
void drawText(const String& t, int x, int y, const GFXfont* f = &FontRu30);
uint16_t xptRead(uint8_t cmd) {
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_DIN, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
  }
  uint16_t v = 0;
  for (int i = 0; i < 12; i++) {
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
    v = (v << 1) | digitalRead(T_DO);
  }
  digitalWrite(T_CS, HIGH);
  return v;
}

bool calSwap = false;
float calUx0, calUk, calVy0, calVk;

bool readRaw(uint16_t* rx, uint16_t* ry) {
  if (xptRead(0xB0) < TOUCH_Z_MIN) return false;
  uint32_t ax = 0, ay = 0;
  for (int i = 0; i < 4; i++) {
    xptRead(0xD0);  // первое измерение после смены канала выбрасываем
    ax += xptRead(0xD0);
    xptRead(0x90);
    ay += xptRead(0x90);
  }
  *rx = ax / 4;
  *ry = ay / 4;
  return true;
}

bool getTouchXY(uint16_t* x, uint16_t* y) {
  uint16_t rx, ry;
  if (!readRaw(&rx, &ry)) return false;
  float u = calSwap ? ry : rx;
  float v = calSwap ? rx : ry;
  *x = constrain((int)((u - calUx0) * calUk + 30), 0, 479);
  *y = constrain((int)((v - calVy0) * calVk + 30), 0, 319);
  return true;
}

void calPoint(int sx, int sy, float* ox, float* oy) {
  tft.fillScreen(TFT_BLACK);
  tft.drawFastHLine(sx - 15, sy, 31, TFT_WHITE);
  tft.drawFastVLine(sx, sy - 15, 31, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  drawText("Нажми на крестик", 240, 160);
  uint16_t rx, ry;
  while (!readRaw(&rx, &ry)) {}
  float sumx = 0, sumy = 0; int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 400) {
    if (readRaw(&rx, &ry)) { sumx += rx; sumy += ry; n++; }
  }
  *ox = sumx / max(n, 1);
  *oy = sumy / max(n, 1);
  while (readRaw(&rx, &ry)) {}
  delay(300);
}

void calibrateTouch() {
  while (true) {
    float x0, y0, x1, y1, x2, y2;
    calPoint(30, 30, &x0, &y0);
    calPoint(450, 30, &x1, &y1);
    calPoint(30, 290, &x2, &y2);
    Serial.printf("RAW p0=%.0f,%.0f p1=%.0f,%.0f p2=%.0f,%.0f\n", x0, y0, x1, y1, x2, y2);
    calSwap = fabsf(y1 - y0) > fabsf(x1 - x0);
    float u0 = calSwap ? y0 : x0, u1 = calSwap ? y1 : x1;
    float v0 = calSwap ? x0 : y0, v2 = calSwap ? x2 : y2;
    if (fabsf(u1 - u0) < 800 || fabsf(v2 - v0) < 800) {  // нажатия слишком близко / не в крестики
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED);
      tft.setTextDatum(MC_DATUM);
      drawText("Ошибка. Повтори", 240, 160);
      delay(1500);
      continue;
    }
    calUx0 = u0; calUk = 420.0f / (u1 - u0);
    calVy0 = v0; calVk = 260.0f / (v2 - v0);
    Serial.printf("CAL swap=%d uk=%.4f vk=%.4f\n", calSwap, calUk, calVk);
    return;
  }
}

// ---------------- Интерфейс ----------------
void drawText(const String& t, int x, int y, const GFXfont* f) {
  tft.setFreeFont(f);
  tft.drawString(t, x, y);
}

struct Btn { int x, y, w, h; };
const Btn btnStart = {16, 236, 300, 68};
const Btn btnStop  = {332, 236, 132, 68};

#define C(r, g, b) tft.color565(r, g, b)
const uint16_t COL_BG = C(10, 14, 26);
const uint16_t COL_HEADER = C(20, 27, 46);
const uint16_t COL_CARD = C(24, 32, 54);
const uint16_t COL_PILL = C(38, 48, 74);
const uint16_t COL_MUTED = C(130, 142, 170);
const uint16_t COL_ACCENT = C(255, 145, 30);
const uint16_t COL_BLUE = C(59, 130, 246);
const uint16_t COL_CYAN = C(34, 211, 238);
const uint16_t COL_GREEN = C(34, 197, 94);
const uint16_t COL_RED = C(239, 68, 68);

bool uiDirty = true;

bool canStart() { return stage == IDLE || stage == WAIT_CONFIRM; }

uint16_t stageColor() {
  switch (stage) {
    case CONVEYOR: return COL_BLUE;
    case SCREW_DOWN: return COL_ACCENT;
    case SCREW_UP: return COL_CYAN;
    case DONE: return COL_GREEN;
    default: return COL_ACCENT;
  }
}

// номер этапа цикла: 0 конвейер, 1 ШВП вниз, 2 ШВП вверх, -1 вне цикла
int stageIndex() {
  switch (stage) {
    case CONVEYOR: return 0;
    case SCREW_DOWN: return 1;
    case SCREW_UP: return 2;
    case DONE: return 3;
    default: return -1;
  }
}

void drawButton(const Btn& b, uint16_t color, const char* label, uint16_t textColor = TFT_WHITE) {
  tft.fillRoundRect(b.x, b.y + 4, b.w, b.h, 16, C(5, 8, 16));  // тень
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 16, color);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 16, C(255, 255, 255));
  tft.setTextColor(textColor);
  tft.setTextDatum(MC_DATUM);
  drawText(label, b.x + b.w / 2, b.y + b.h / 2, &FontRu30);
}

float progress() {
  switch (stage) {
    case CONVEYOR: return constrain((millis() - stageStart) / (float)CONVEYOR_LOOP_MS, 0.f, 1.f);
    case SCREW_DOWN:
    case SCREW_UP: return 1.0f - stepsLeft / (float)SCREW_STEPS;
    case DONE: return 1.0f;
    default: return 0.0f;
  }
}

void drawProgress() {
  const int x = 16, y = 216, w = 448, h = 10;
  int filled = (int)(w * progress());
  tft.fillRoundRect(x, y, w, h, 5, COL_PILL);
  if (filled > 10) tft.fillRoundRect(x, y, filled, h, 5, stageColor());
}

// Иконка этапа в круге (cx, cy, радиус 44). phase — угол анимации конвейера
void drawIcon(int cx, int cy, int phase) {
  const int R = 44;
  uint16_t col = stageColor();
  tft.fillCircle(cx, cy, R + 2, COL_CARD);
  tft.fillCircle(cx, cy, R, C(14, 20, 36));
  tft.drawCircle(cx, cy, R, col);
  tft.drawCircle(cx, cy, R - 1, col);
  switch (stage) {
    case CONVEYOR:
      tft.drawArc(cx, cy, 30, 22, (phase) % 360, (phase + 120) % 360, col, C(14, 20, 36), false);
      tft.drawArc(cx, cy, 30, 22, (phase + 180) % 360, (phase + 300) % 360, col, C(14, 20, 36), false);
      break;
    case SCREW_DOWN:
      tft.fillRect(cx - 7, cy - 26, 14, 28, col);
      tft.fillTriangle(cx - 22, cy, cx + 22, cy, cx, cy + 26, col);
      break;
    case SCREW_UP:
      tft.fillRect(cx - 7, cy - 2, 14, 28, col);
      tft.fillTriangle(cx - 22, cy, cx + 22, cy, cx, cy - 26, col);
      break;
    case DONE:
      for (int i = -2; i <= 2; i++) {
        tft.drawLine(cx - 22, cy + i, cx - 6, cy + 16 + i, col);
        tft.drawLine(cx - 6, cy + 16 + i, cx + 24, cy - 16 + i, col);
      }
      break;
    default:  // IDLE / WAIT_CONFIRM: «play»
      tft.fillTriangle(cx - 12, cy - 22, cx - 12, cy + 22, cx + 24, cy, col);
      break;
  }
}

void drawStatusDots() {
  tft.fillRect(270, 8, 206, 40, COL_HEADER);
  tft.setTextColor(COL_MUTED);
  tft.setTextDatum(ML_DATUM);
  bool wifi = WiFi.status() == WL_CONNECTED;
  tft.fillCircle(290, 28, 7, wifi ? COL_GREEN : COL_RED);
  drawText("Wi-Fi", 302, 28, &FontRu20);
  tft.fillCircle(386, 28, 7, mqtt.connected() ? COL_GREEN : COL_RED);
  drawText("MQTT", 398, 28, &FontRu20);
}

String cutLabel() {
  if (stepCut == "sticks") return "Брусочки";
  if (stepCut == "shred") return "Шинковка";
  if (stepCut == "grate") return "Тёрка";
  return stepCut;
}

void drawUI() {
  tft.fillScreen(COL_BG);

  // шапка
  tft.fillRect(0, 0, 480, 52, COL_HEADER);
  tft.fillRect(0, 52, 480, 3, COL_ACCENT);
  tft.fillRoundRect(14, 8, 38, 38, 10, COL_ACCENT);
  tft.setTextColor(COL_BG);
  tft.setTextDatum(MC_DATUM);
  drawText("S", 33, 28, &FontRu30);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  drawText("SmartCut", 62, 28, &FontRu30);
  drawStatusDots();

  // карточка этапа
  tft.fillRoundRect(16, 64, 448, 112, 18, COL_CARD);
  drawIcon(78, 120, 0);

  const char* title = "";
  switch (stage) {
    case IDLE: title = "Готов к работе"; break;
    case WAIT_CONFIRM: title = "Подтверди старт"; break;
    case CONVEYOR: title = "Конвейер"; break;
    case SCREW_DOWN: title = "ШВП вниз"; break;
    case SCREW_UP: title = "ШВП вверх"; break;
    case DONE: title = "Готово!"; break;
  }
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  drawText(title, 144, 100, &FontRu30);

  String sub;
  if (stage == IDLE) {
    sub = "Нажми «Начать нарезку»";
  } else if (stage == DONE) {
    sub = "Нарезка завершена";
  } else if (stepCut.length()) {
    sub = stepProduct.length() ? stepProduct + " · " + cutLabel() : cutLabel();
    if (stepGrams > 0) sub += " · " + String(stepGrams) + " г";
  } else {
    sub = "Этап " + String(stageIndex() + 1) + " из 3";
  }
  tft.setTextColor(COL_MUTED);
  drawText(sub, 144, 140, &FontRu20);

  // этапы цикла
  const char* names[3] = {"Конвейер", "ШВП вниз", "ШВП вверх"};
  int cur = stageIndex();
  for (int i = 0; i < 3; i++) {
    int x = 16 + i * 152;
    uint16_t bg = cur > i ? C(22, 101, 52) : (cur == i ? stageColor() : COL_PILL);
    tft.fillRoundRect(x, 184, 144, 26, 13, bg);
    tft.setTextColor(cur >= i ? TFT_WHITE : COL_MUTED);
    tft.setTextDatum(MC_DATUM);
    drawText(names[i], x + 72, 197, &FontRu20);
  }
  drawProgress();

  bool run = !canStart() && stage != DONE;
  uint16_t startCol = (run || stage == DONE) ? COL_PILL : COL_GREEN;
  drawButton(btnStart, startCol, stage == WAIT_CONFIRM ? "Подтвердить" : "Начать нарезку",
             (run || stage == DONE) ? COL_MUTED : TFT_WHITE);
  drawButton(btnStop, COL_RED, "СТОП");
  uiDirty = false;
}

void animateIcon() {
  static uint32_t last = 0;
  if (stage != CONVEYOR || millis() - last < 120) return;
  last = millis();
  drawIcon(78, 120, (millis() / 6) % 360);
}

// ---------------- Управление циклом ----------------
void setStage(Stage s) {
  stage = s;
  stageStart = millis();
  uiDirty = true;
  publishStatus();
}

void startCycle() {
  if (!canStart()) return;
  conveyorOn();
  setStage(CONVEYOR);
}

void stopAll() {
  conveyorOff();
  stepsLeft = 0;
  releaseScrew();
  stepProduct = ""; stepCut = ""; stepGrams = -1;
  setStage(IDLE);
}

void tickCycle() {
  switch (stage) {
    case CONVEYOR:
      if (millis() - stageStart >= CONVEYOR_LOOP_MS) {
        conveyorOff();
        screwDir = SCREW_DOWN_DIR;
        stepsLeft = SCREW_STEPS;
        setStage(SCREW_DOWN);
      }
      break;
    case SCREW_DOWN:
      if (stepsLeft <= 0) {
        screwDir = -SCREW_DOWN_DIR;
        stepsLeft = SCREW_STEPS;
        setStage(SCREW_UP);
      }
      break;
    case SCREW_UP:
      if (stepsLeft <= 0) {
        releaseScrew();
        setStage(DONE);
      }
      break;
    case DONE:
      if (millis() - stageStart > 3000) {
        stepProduct = ""; stepCut = ""; stepGrams = -1;
        setStage(IDLE);
      }
      break;
    default: break;
  }
}

void handleTouch() {
  static uint32_t lastPoll = 0, lastPress = 0;
  if (millis() - lastPoll < 30) return;
  lastPoll = millis();
  uint16_t x, y;
  if (!getTouchXY(&x, &y) || millis() - lastPress < 400) return;
  lastPress = millis();
  Serial.printf("TOUCH x=%u y=%u\n", x, y);
  auto hit = [&](const Btn& b) { return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h; };
  if (hit(btnStop)) stopAll();
  else if (hit(btnStart)) startCycle();
}

// ---------------- MQTT ----------------
void onMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  String action = d["action"] | "";
  if (action == "recipe_step") {
    if (!canStart()) return;
    stepProduct = (const char*)(d["product"] | "");
    stepCut = (const char*)(d["cut"] | "");
    stepGrams = d["grams"] | -1;
    stepAuto = d["autoconfirm"] | false;
    setStage(WAIT_CONFIRM);
    if (stepAuto) startCycle();
  } else if (action == "clear_step") {
    if (stage == WAIT_CONFIRM) stopAll();
  } else if (action == "stop") {
    stopAll();
  }
}

void networkTick() {
  static uint32_t lastDots = 0;
  if (mqtt.connected()) {
    mqtt.loop();
    if (millis() - lastStatus > 5000) publishStatus();
  } else if (canStart() && WiFi.status() == WL_CONNECTED && millis() - lastMqttTry > 5000) {
    lastMqttTry = millis();
    if (mqtt.connect(("smartcut-esp-" + String((uint32_t)ESP.getEfuseMac(), HEX)).c_str())) {
      mqtt.subscribe(topicCmd.c_str());
      publishStatus();
      uiDirty = true;
    }
  }
  if (millis() - lastDots > 2000) {
    lastDots = millis();
    if (canStart()) drawStatusDots();
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  const int outs[] = {IN1, IN2, IN3, IN4, ST1, ST2, ST3, ST4};
  for (int p : outs) pinMode(p, OUTPUT);
  ledcSetup(0, 2000, 8);
  ledcAttachPin(ENA, 0);
  ledcSetup(1, 2000, 8);
  ledcAttachPin(ENB, 1);
  conveyorOff();
  releaseScrew();

  tft.init();
  tft.setRotation(1);
  pinMode(T_CLK, OUTPUT);
  pinMode(T_CS, OUTPUT);
  pinMode(T_DIN, OUTPUT);
  pinMode(T_DO, INPUT);
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_CLK, LOW);
  calibrateTouch();

  String id = DEVICE_ID;
  String base = id.length() ? "smartcut/" + id : "smartcut";
  topicCmd = base + "/cmd";
  topicStatus = base + "/status";
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(2);
  mqtt.setCallback(onMessage);

  drawUI();
}

void loop() {
  handleTouch();
  stepMotor();
  tickCycle();
  networkTick();
  if (uiDirty) drawUI();
  else if (stage != IDLE && stage != WAIT_CONFIRM) {
    static uint32_t lastP = 0;
    if (millis() - lastP > 250) { lastP = millis(); drawProgress(); }
    animateIcon();
  }
}
