#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_timer.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#include "font_ru.h"

TFT_eSPI tft;

// параметры цикла
#define CONVEYOR_LOOP_MS 5000    // один круг конвейера, мс
#define CONVEYOR_SPEED 165       // 0..255
#define SCREW_STEPS 81000        // шагов от верха до низа
#define SCREW_DOWN_DIR -1        // 1 или -1
#define MANUAL_JOG 1             // кнопки «Вверх»/«Вниз» на экране
#define START_DELAY_US 3000      // пауза между шагами на старте, мкс
#define RAMP_STEPS 1200          // шагов разгона и торможения
#define STEP_DELAY_US 600        // пауза между шагами, мкс

// Пины
// конвейер (L298N)
#define ENA 15
#define IN1 2
#define IN2 4
#define IN3 16
#define IN4 17
#define ENB 5
// ШВП (NEMA 17)
#define ST1 13
#define ST2 12
#define ST3 14
#define ST4 27
// тач XPT2046
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

// Состояние цикла
enum Stage { IDLE, WAIT_CONFIRM, CONVEYOR, SCREW_DOWN, SCREW_UP, DONE, RETURN_HOME };
Stage stage = IDLE;
uint32_t stageStart = 0;
volatile long stepsLeft = 0;
volatile int screwDir = 1;

// положение ШВП: шагов вниз от верха, хранится в памяти
#define HOME_MARGIN_STEPS 500
Preferences prefs;
volatile long screwPos = 0;
long homeTotal = 1;

String stepProduct, stepCut;
int stepGrams = -1;
bool stepAuto = false;

// MQTT
WiFiManager wm;
bool portalMode = false;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String topicCmd, topicStatus, topicProduct;
String detectedProduct;
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

// Моторы
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

// шаги идут от таймера, чтобы экран и тач не сбивали ритм
volatile long stepsDone = 0;
volatile bool stepActive = false;
portMUX_TYPE stepMux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t stepTimer;

void onStepTimer(void*) {
  static uint8_t idx = 0;
  static int lastDir = 0;
  portENTER_CRITICAL(&stepMux);
  long left = stepsLeft;
  int dir = screwDir;
  if (left <= 0) {
    stepsDone = 0;
    lastDir = 0;
    stepActive = false;
    portEXIT_CRITICAL(&stepMux);
    releaseScrew();
    return;
  }
  if (dir != lastDir) {
    stepsDone = 0;
    lastDir = dir;
  }
  idx = (idx + (dir > 0 ? 1 : 3)) & 3;
  stepsLeft = left - 1;
  stepsDone = stepsDone + 1;
  long pos = screwPos + ((dir == SCREW_DOWN_DIR) ? 1 : -1);
  screwPos = pos < 0 ? 0 : pos;
  long n = min((long)stepsDone, left - 1);
  portEXIT_CRITICAL(&stepMux);

  digitalWrite(ST1, stepSeq[idx][0]);
  digitalWrite(ST2, stepSeq[idx][1]);
  digitalWrite(ST3, stepSeq[idx][2]);
  digitalWrite(ST4, stepSeq[idx][3]);

  uint32_t d = STEP_DELAY_US;
  if (n < RAMP_STEPS) d += (RAMP_STEPS - n) * (START_DELAY_US - STEP_DELAY_US) / RAMP_STEPS;
  esp_timer_start_once(stepTimer, d);
}

void startMove(int dir, long steps) {
  portENTER_CRITICAL(&stepMux);
  screwDir = dir;
  stepsLeft = steps;
  bool kick = !stepActive;
  stepActive = true;
  portEXIT_CRITICAL(&stepMux);
  if (kick) esp_timer_start_once(stepTimer, START_DELAY_US);
}

void savePos(bool moving) {
  prefs.putLong("pos", screwPos);
  prefs.putBool("moving", moving);
}

void loadPos() {
  prefs.begin("cut", false);
  screwPos = prefs.getLong("pos", 0);
  // питание пропало в движении: считаем, что нож внизу
  if (prefs.getBool("moving", false)) screwPos = SCREW_STEPS;
}

// Тач
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
    xptRead(0xD0);  // первое измерение отбрасываем
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

void saveCal() {
  prefs.putBool("cal2_ok", true);
  prefs.putBool("cal_swap", calSwap);
  prefs.putFloat("cal_ux0", calUx0);
  prefs.putFloat("cal_uk", calUk);
  prefs.putFloat("cal_vy0", calVy0);
  prefs.putFloat("cal_vk", calVk);
}

bool loadCal() {
  if (!prefs.getBool("cal2_ok", false)) return false;
  calSwap = prefs.getBool("cal_swap", false);
  calUx0 = prefs.getFloat("cal_ux0", 0);
  calUk = prefs.getFloat("cal_uk", 0);
  calVy0 = prefs.getFloat("cal_vy0", 0);
  calVk = prefs.getFloat("cal_vk", 0);
  return calUk != 0 && calVk != 0;
}

// ждём, пока палец убран
void waitRelease() {
  uint16_t rx, ry;
  uint32_t t = millis();
  while (millis() - t < 300) {
    if (readRaw(&rx, &ry)) t = millis();
  }
}

void calPoint(int sx, int sy, float* ox, float* oy) {
  tft.fillScreen(tft.color565(238, 242, 248));
  tft.drawFastHLine(sx - 15, sy, 31, tft.color565(25, 34, 56));
  tft.drawFastVLine(sx, sy - 15, 31, tft.color565(25, 34, 56));
  tft.setTextColor(tft.color565(25, 34, 56));
  tft.setTextDatum(MC_DATUM);
  drawText("Нажми на крестик", 240, 160);
  waitRelease();
  uint16_t rx, ry;
  while (!readRaw(&rx, &ry)) {}
  delay(150);
  float sumx = 0, sumy = 0; int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 350) {
    if (readRaw(&rx, &ry)) { sumx += rx; sumy += ry; n++; }
  }
  *ox = sumx / max(n, 1);
  *oy = sumy / max(n, 1);
  waitRelease();
}

void calibrateTouch() {
  while (true) {
    float x0, y0, x1, y1, x2, y2;
    calPoint(30, 30, &x0, &y0);
    calPoint(450, 30, &x1, &y1);
    calPoint(30, 290, &x2, &y2);
    calSwap = fabsf(y1 - y0) > fabsf(x1 - x0);
    float u0 = calSwap ? y0 : x0, u1 = calSwap ? y1 : x1;
    float v0 = calSwap ? x0 : y0, v2 = calSwap ? x2 : y2;
    if (fabsf(u1 - u0) < 800 || fabsf(v2 - v0) < 800) {
      tft.fillScreen(tft.color565(238, 242, 248));
      tft.setTextColor(TFT_RED);
      tft.setTextDatum(MC_DATUM);
      drawText("Ошибка. Повтори", 240, 160);
      delay(1500);
      continue;
    }
    calUx0 = u0; calUk = 420.0f / (u1 - u0);
    calVy0 = v0; calVk = 260.0f / (v2 - v0);
    // проверка по четвёртой точке
    float x3, y3;
    calPoint(450, 290, &x3, &y3);
    float pu = calSwap ? y3 : x3, pv = calSwap ? x3 : y3;
    float sx = (pu - calUx0) * calUk + 30, sy = (pv - calVy0) * calVk + 30;
    if (fabsf(sx - 450) > 45 || fabsf(sy - 290) > 45) {
      tft.fillScreen(tft.color565(238, 242, 248));
      tft.setTextColor(TFT_RED);
      tft.setTextDatum(MC_DATUM);
      drawText("Ошибка. Повтори", 240, 160);
      delay(1500);
      continue;
    }
    saveCal();
    return;
  }
}

// Интерфейс
void drawText(const String& text, int x, int y, const GFXfont* f) {
  String t = text;
  t.replace("ё", "е");
  t.replace("Ё", "Е");
  tft.setFreeFont(f);
  tft.drawString(t, x, y);
}

struct Btn { int x, y, w, h; };
const Btn btnStart = {16, 236, 300, 68};
const Btn btnStop  = {332, 236, 132, 68};
#if MANUAL_JOG
const Btn btnUp   = {16, 184, 220, 40};
const Btn btnDown = {244, 184, 220, 40};
#endif

#define C(r, g, b) tft.color565(r, g, b)
const uint16_t COL_BG = C(238, 242, 248);
const uint16_t COL_HEADER = C(255, 255, 255);
const uint16_t COL_CARD = C(255, 255, 255);
const uint16_t COL_BORDER = C(210, 217, 230);
const uint16_t COL_ICONBG = C(240, 244, 250);
const uint16_t COL_PILL = C(224, 230, 240);
const uint16_t COL_SHADOW = C(200, 207, 220);
const uint16_t COL_TEXT = C(25, 34, 56);
const uint16_t COL_MUTED = C(107, 118, 140);
const uint16_t COL_ACCENT = C(245, 130, 20);
const uint16_t COL_BLUE = C(37, 99, 235);
const uint16_t COL_CYAN = C(8, 145, 178);
const uint16_t COL_GREEN = C(22, 163, 74);
const uint16_t COL_RED = C(220, 38, 38);

bool uiDirty = true;

bool canStart() { return stage == IDLE || stage == WAIT_CONFIRM; }

uint16_t stageColor() {
  switch (stage) {
    case CONVEYOR: return COL_BLUE;
    case SCREW_DOWN: return COL_ACCENT;
    case SCREW_UP:
    case RETURN_HOME: return COL_CYAN;
    case DONE: return COL_GREEN;
    default: return COL_ACCENT;
  }
}

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
  tft.fillRoundRect(b.x, b.y + 4, b.w, b.h, 16, COL_SHADOW);
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 16, color);
  tft.setTextColor(textColor);
  tft.setTextDatum(MC_DATUM);
  drawText(label, b.x + b.w / 2, b.y + b.h / 2, &FontRu30);
}

float progress() {
  switch (stage) {
    case CONVEYOR: return constrain((millis() - stageStart) / (float)CONVEYOR_LOOP_MS, 0.f, 1.f);
    case SCREW_DOWN:
    case SCREW_UP: return 1.0f - stepsLeft / (float)SCREW_STEPS;
    case RETURN_HOME: return 1.0f - stepsLeft / (float)homeTotal;
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

void drawIcon(int cx, int cy, int phase) {
  const int R = 44;
  uint16_t col = stageColor();
  tft.fillCircle(cx, cy, R + 2, COL_CARD);
  tft.fillCircle(cx, cy, R, COL_ICONBG);
  tft.drawCircle(cx, cy, R, col);
  tft.drawCircle(cx, cy, R - 1, col);
  switch (stage) {
    case CONVEYOR:
      tft.drawArc(cx, cy, 30, 22, (phase) % 360, (phase + 120) % 360, col, COL_ICONBG, false);
      tft.drawArc(cx, cy, 30, 22, (phase + 180) % 360, (phase + 300) % 360, col, COL_ICONBG, false);
      break;
    case SCREW_DOWN:
      tft.fillRect(cx - 7, cy - 26, 14, 28, col);
      tft.fillTriangle(cx - 22, cy, cx + 22, cy, cx, cy + 26, col);
      break;
    case SCREW_UP:
    case RETURN_HOME:
      tft.fillRect(cx - 7, cy - 2, 14, 28, col);
      tft.fillTriangle(cx - 22, cy, cx + 22, cy, cx, cy - 26, col);
      break;
    case DONE:
      for (int i = -2; i <= 2; i++) {
        tft.drawLine(cx - 22, cy + i, cx - 6, cy + 16 + i, col);
        tft.drawLine(cx - 6, cy + 16 + i, cx + 24, cy - 16 + i, col);
      }
      break;
    default:
      tft.fillTriangle(cx - 12, cy - 22, cx - 12, cy + 22, cx + 24, cy, col);
      break;
  }
}

String cutLabel() {
  if (stepCut == "sticks") return "Брусочки";
  if (stepCut == "shred") return "Шинковка";
  if (stepCut == "grate") return "Тёрка";
  return stepCut;
}

void drawUI() {
  tft.fillScreen(COL_BG);

  tft.fillRect(0, 0, 480, 52, COL_HEADER);
  tft.fillRect(0, 52, 480, 3, COL_ACCENT);
  tft.fillRoundRect(14, 8, 38, 38, 10, COL_ACCENT);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  drawText("S", 33, 28, &FontRu30);
  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(ML_DATUM);
  drawText("SmartCut", 62, 28, &FontRu30);

  tft.fillRoundRect(16, 64, 448, 112, 18, COL_CARD);
  tft.drawRoundRect(16, 64, 448, 112, 18, COL_BORDER);
  drawIcon(78, 120, 0);

  const char* title = "";
  switch (stage) {
    case IDLE: title = "Готов к работе"; break;
    case WAIT_CONFIRM: title = "Подтверди старт"; break;
    case CONVEYOR: title = "Двигаем продукт"; break;
    case SCREW_DOWN: title = "Продукт нарезается"; break;
    case SCREW_UP: title = "Возвращаем в исходное"; break;
    case RETURN_HOME: title = "Подготовка"; break;
    case DONE: title = "Готово!"; break;
  }
  tft.setFreeFont(&FontRu30);
  const GFXfont* titleFont = tft.textWidth(title) > 310 ? &FontRu20 : &FontRu30;
  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(ML_DATUM);
  drawText(title, 144, 100, titleFont);

  String sub;
  if (stage == IDLE) {
    if (portalMode && WiFi.status() != WL_CONNECTED) sub = "Wi-Fi: сеть SmartCut-Setup";
    else sub = detectedProduct.length() ? "На ленте: " + detectedProduct : "Нажми «Начать нарезку»";
  } else if (stage == DONE) {
    sub = "Нарезка завершена";
  } else if (stage == RETURN_HOME) {
    sub = "Возвращаем в исходное состояние";
  } else if (stepCut.length()) {
    sub = stepProduct.length() ? stepProduct + " · " + cutLabel() : cutLabel();
    if (stepGrams > 0) sub += " · " + String(stepGrams) + " г";
  } else {
    sub = "Этап " + String(stageIndex() + 1) + " из 3";
  }
  tft.setTextColor(COL_MUTED);
  drawText(sub, 144, 140, &FontRu20);

#if MANUAL_JOG
  if (stage == IDLE) {
    drawButton(btnUp, COL_BLUE, "Вверх");
    drawButton(btnDown, COL_ACCENT, "Вниз");
  }
#endif
  const char* names[3] = {"Подача", "Нарезка", "Возврат"};
  int cur = stageIndex();
  for (int i = 0; i < 3; i++) {
#if MANUAL_JOG
    if (stage == IDLE) break;
#endif
    int x = 16 + i * 152;
    uint16_t bg = cur > i ? COL_GREEN : (cur == i ? stageColor() : COL_PILL);
    tft.fillRoundRect(x, 184, 144, 26, 13, bg);
    tft.setTextColor(cur >= i ? TFT_WHITE : COL_MUTED);
    tft.setTextDatum(MC_DATUM);
    drawText(names[i], x + 72, 197, &FontRu20);
  }
#if MANUAL_JOG
  if (stage != IDLE)
#endif
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

// Управление циклом
void setStage(Stage s) {
  stage = s;
  stageStart = millis();
  uiDirty = true;
  publishStatus();
}

#if MANUAL_JOG
void stopJog();
#endif

void startCycle() {
  if (!canStart()) return;
  if (screwPos > 0) {
    homeTotal = screwPos + HOME_MARGIN_STEPS;
    savePos(true);
    startMove(-SCREW_DOWN_DIR, homeTotal);
    setStage(RETURN_HOME);
    return;
  }
  conveyorOn();
  setStage(CONVEYOR);
}

void stopAll() {
#if MANUAL_JOG
  stopJog();
#endif
  Stage was = stage;
  conveyorOff();
  stepsLeft = 0;
  releaseScrew();
  if (was == SCREW_DOWN || was == SCREW_UP || was == RETURN_HOME) savePos(false);
  stepProduct = ""; stepCut = ""; stepGrams = -1;
  setStage(IDLE);
}

void tickCycle() {
  switch (stage) {
    case RETURN_HOME:
      if (stepsLeft <= 0) {
        screwPos = 0;
        releaseScrew();
        savePos(false);
        conveyorOn();
        setStage(CONVEYOR);
      }
      break;
    case CONVEYOR:
      if (millis() - stageStart >= CONVEYOR_LOOP_MS) {
        conveyorOff();
        savePos(true);
        startMove(SCREW_DOWN_DIR, SCREW_STEPS);
        setStage(SCREW_DOWN);
      }
      break;
    case SCREW_DOWN:
      if (stepsLeft <= 0) {
        startMove(-SCREW_DOWN_DIR, SCREW_STEPS);
        setStage(SCREW_UP);
      }
      break;
    case SCREW_UP:
      if (stepsLeft <= 0) {
        releaseScrew();
        screwPos = 0;
        savePos(false);
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

#if MANUAL_JOG
// ручное движение: нажал — едет, нажал ещё раз — стоп
bool jogging = false;
int jogWant = 0;

void stopJog() {
  if (!jogging) return;
  jogging = false;
  stepsLeft = 0;
  releaseScrew();
  savePos(false);
}

void updateJog(bool touched, uint16_t x, uint16_t y) {
  static uint32_t lastTouchMs = 0;
  static bool wasTouched = false;
  auto in = [&](const Btn& b) { return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h; };

  if (jogging && (stage != IDLE || stepsLeft <= 0)) stopJog();

  bool press = touched && !wasTouched && millis() - lastTouchMs > 150;
  if (touched) lastTouchMs = millis();
  wasTouched = touched;
  if (!press || stage != IDLE) return;

  int dir = 0;
  if (in(btnUp)) dir = -SCREW_DOWN_DIR;
  else if (in(btnDown)) dir = SCREW_DOWN_DIR;
  if (dir == 0) return;

  if (jogging && screwDir == dir) {
    stopJog();
    return;
  }
  savePos(true);
  startMove(dir, 100000000L);
  jogging = true;
}
#endif

void handleTouch() {
  static uint32_t lastPoll = 0, lastPress = 0;
  if (millis() - lastPoll < 30) return;
  lastPoll = millis();
  uint16_t x, y;
  bool touched = getTouchXY(&x, &y);
#if MANUAL_JOG
  updateJog(touched, x, y);
#endif
  if (!touched || millis() - lastPress < 400) return;
  lastPress = millis();
  auto hit = [&](const Btn& b) { return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h; };
  if (hit(btnStop)) stopAll();
  else if (hit(btnStart)) startCycle();
}

// MQTT
void onMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument d;
  if (String(topic) == topicProduct) {
    String name;
    if (!deserializeJson(d, payload, len)) name = (const char*)(d["product"] | "");
    if (name.equalsIgnoreCase("none")) name = "";
    if (name != detectedProduct) {
      detectedProduct = name;
      if (stage == IDLE) uiDirty = true;
    }
    return;
  }
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
  if (mqtt.connected()) {
    mqtt.loop();
    if (millis() - lastStatus > 5000) publishStatus();
  } else if (canStart() && WiFi.status() == WL_CONNECTED && millis() - lastMqttTry > 5000) {
    lastMqttTry = millis();
    if (mqtt.connect(("smartcut-esp-" + String((uint32_t)ESP.getEfuseMac(), HEX)).c_str())) {
      mqtt.subscribe(topicCmd.c_str());
      mqtt.subscribe(topicProduct.c_str());
      publishStatus();
    }
  }
}

// setup / loop
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
  loadPos();
  const esp_timer_create_args_t timerArgs = {.callback = &onStepTimer, .name = "step"};
  esp_timer_create(&timerArgs, &stepTimer);

  tft.init();
  tft.setRotation(1);
  pinMode(T_CLK, OUTPUT);
  pinMode(T_CS, OUTPUT);
  pinMode(T_DIN, OUTPUT);
  pinMode(T_DO, INPUT);
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_CLK, LOW);
  // калибровка запоминается; чтобы повторить, держите палец на экране при включении
  tft.fillScreen(tft.color565(238, 242, 248));
  tft.setTextColor(tft.color565(25, 34, 56));
  tft.setTextDatum(MC_DATUM);
  drawText("SmartCut", 240, 130);
  drawText("Удерживай экран для калибровки", 240, 180, &FontRu20);
  bool held = false;
  uint32_t tb = millis();
  uint16_t hx, hy;
  while (millis() - tb < 1800) {
    if (readRaw(&hx, &hy)) { held = true; break; }
  }
  if (held || !loadCal()) calibrateTouch();

  String id = DEVICE_ID;
  String base = id.length() ? "smartcut/" + id : "smartcut";
  topicCmd = base + "/cmd";
  topicStatus = base + "/status";
  topicProduct = base + "/product";
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID)) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  } else {
    portalMode = true;
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(15);
  }
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(2);
  mqtt.setCallback(onMessage);

  drawUI();
  if (portalMode) wm.autoConnect("SmartCut-Setup");
}

void loop() {
  if (portalMode) {
    wm.process();
    static bool wasConnected = false;
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wasConnected) {
      wasConnected = connected;
      if (stage == IDLE) uiDirty = true;
    }
  }
  handleTouch();
  tickCycle();
  networkTick();
  if (uiDirty) drawUI();
  else if (stage != IDLE && stage != WAIT_CONFIRM) {
    static uint32_t lastP = 0;
    if (millis() - lastP > 250) { lastP = millis(); drawProgress(); }
    animateIcon();
  }
}
