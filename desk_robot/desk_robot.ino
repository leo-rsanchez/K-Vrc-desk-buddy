/*
  Desk robot - animated eyes  -  XIAO ESP32-C6 + OLED 128x64 (I2C) + BMI160 + TTP223

  Wiring (see diagram):
    OLED   SDA -> D4   SCL -> D5   VCC -> 3V3   GND -> GND
    BMI160 SDA -> D4   SCL -> D5   VCC -> 3V3   GND -> GND   CS -> 3V3   SDO -> GND (0x68)
    TTP223 OUT -> D1   VCC -> 3V3  GND -> GND

  Required library (Library Manager): "U8g2" by olikraus.
  Board: "XIAO_ESP32C6" (Espressif esp32 package).

  States: AWAKE <-> CURIOUS (idle) - HAPPY (touch) - DIZZY (shake / lift) - SLEEPING (1 min without touch or motion)
  No delay() in loop: everything uses millis() and a step queue.
  Eyes are drawn in code with the same shapes/timings as the 40 frames.
*/
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>

// --------- Display: uncomment the line if your OLED is an SH1106 (the 1.3" one) ---------
// #define OLED_SH1106
#ifdef OLED_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

// --------- Pins and parameters ---------
const uint8_t PIN_TOUCH = 1;           // GPIO1 = XIAO D1. TTP223: HIGH while touched
const uint8_t PIN_SDA   = 22;          // GPIO22 = XIAO D4
const uint8_t PIN_SCL   = 23;          // GPIO23 = XIAO D5
const uint32_t IDLE_SLEEP_MS     = 60000;  // 1 min without touch or motion -> sleep
const uint16_t TOUCH_DEBOUNCE_MS = 50;
// Sensitivity: lifting the board makes it dizzy, a small tap on the desk does not.
//   Gets dizzy too easily      -> raise SHAKE_G (0.25, 0.30...) or set SHAKE_JOLTS to 3.
//   Not dizzy when lifted      -> lower SHAKE_G (0.20, 0.18...).
const float    SHAKE_G           = 0.22f;  // |acceleration - 1g| that counts as a jolt (original 0.8)
const uint8_t  SHAKE_JOLTS       = 2;      // jolts within the window = shake (original 4)
const uint16_t SHAKE_JOLT_GAP_MS = 40;     // minimum gap between counted jolts
const uint16_t SHAKE_WINDOW_MS   = 600;
const uint16_t SHAKE_COOLDOWN_MS = 1200;
const float    MOTION_G          = 0.12f;  // gentle motion: keeps it dizzy and counts as activity
const uint16_t DIZZY_HOLD_MS     = 1000;   // stays dizzy this long after motion stops

// --------- Per-frame timings (ms), generated with generate_eyes.py ---------
enum Anim : uint8_t { ANIM_AWAKE, ANIM_HAPPY, ANIM_DIZZY, ANIM_SLEEPING, ANIM_CURIOUS };
const uint16_t T_MIN[5][8] = {
  {2000, 30, 30, 30, 40, 30, 30, 30},        // AWAKE
  {40, 50, 50, 60, 1000, 70, 70, 80},        // HAPPY
  {70, 90, 140, 200, 220, 220, 200, 350},    // DIZZY
  {1100, 240, 240, 240, 430, 240, 240, 240}, // SLEEPING
  {700, 200, 700, 200, 700, 200, 700, 200},  // CURIOUS
};
const uint16_t T_MAX[5][8] = {
  {5000, 30, 30, 30, 40, 30, 30, 30},
  {40, 50, 50, 60, 2000, 70, 70, 80},
  {70, 90, 140, 200, 220, 220, 200, 350},
  {1500, 280, 280, 280, 530, 280, 280, 280},
  {1500, 400, 1500, 400, 1500, 400, 1500, 400},
};
const char* const ANIM_NAMES[5] = { "AWAKE", "HAPPY", "DIZZY", "SLEEPING", "CURIOUS" };

// ========================= EYE DRAWING (128x64) =========================
// Design coordinates = 480x320 canvas; S scales them down to the OLED.
const float S = 0.40f, OX = 64.0f, OY = 25.0f;           // eyes: scale and on-screen center (was 0.34 / 34)
const float EYE_DX = 66, EYE_W = 88, EYE_H = 100, PRX = 14, PRY = 20;
const float MS = 0.34f, MOUTH_Y = 55.0f;                  // mouth: own scale and fixed on-screen height (px)
const float PI_F = 3.14159265f;

// ---- Types (must come before the first function for Arduino auto-prototyping) ----
struct Eye {
  float w, yl, yt, yb, kx, ky, rot, scale, dy;
  bool hasPupil; float px, py, prx, pry;
};
struct Pt { float x, y; };
enum State : uint8_t { ST_AWAKE, ST_CURIOUS, ST_HAPPY, ST_DIZZY, ST_DROWSY, ST_SLEEPING };
enum Pose  : uint8_t { POSE_C, POSE_L, POSE_R, POSE_U };
struct Step { uint8_t anim, frame; uint16_t ms; };

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static float smoothf(float t) { t = clamp01(t); return t * t * (3 - 2 * t); }

Eye makeEye() { return { EYE_W, 0, -EYE_H / 2, EYE_H / 2, 0.66f, 0.66f, 0, 1, 0, false, 0, 0, 0, 0 }; }

void setPupilFit(Eye &e, float dx, float dy, float rx, float ry, float k) {   // pupil that fits the eye
  float h = e.yb - e.yt, c = (e.yt + e.yb) / 2;
  float fit = smoothf((h - 12) / 22) * k;
  e.hasPupil = fit >= 0.30f;     // at 128x64 a smaller pupil would be a stray pixel
  if (!e.hasPupil) return;
  e.px = dx; e.py = c + dy; e.prx = rx * (0.55f + 0.45f * fit); e.pry = fminf(ry, 0.44f * h) * fit;
}

static Pt toScreen(float lx, float ly, const Eye &e, float cxOff) {   // eye-local -> screen
  float a = e.rot * PI_F / 180.0f, ca = cosf(a), sa = sinf(a);
  float x = lx * e.scale, y = ly * e.scale;
  float rx = x * ca - y * sa + cxOff, ry = x * sa + y * ca + e.dy;
  return { OX + rx * S, OY + ry * S };
}
static Pt bez(Pt a, Pt b, Pt c, Pt d, float t) {
  float u = 1 - t, w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
  return { w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y };
}

void fillPoly(const Pt *p, int n) {                       // scanline fill
  float ymin = 1e9f, ymax = -1e9f;
  for (int i = 0; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
  int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
  if (y0 < 0) y0 = 0;
  if (y1 > 63) y1 = 63;
  for (int y = y0; y <= y1; y++) {
    float yc = y + 0.5f, xs[16]; int c = 0;
    for (int i = 0; i < n; i++) {
      Pt a = p[i], b = p[(i + 1) % n];
      if (((a.y <= yc) && (b.y > yc)) || ((b.y <= yc) && (a.y > yc)))
        if (c < 16) xs[c++] = a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y);
    }
    for (int i = 1; i < c; i++) { float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
    for (int k = 0; k + 1 < c; k += 2) {
      int xa = (int)lroundf(xs[k]), xb = (int)lroundf(xs[k + 1]);
      if (xb > xa) u8g2.drawHLine(xa, y, xb - xa);
    }
  }
}

void drawEye(const Eye &e, float cxOff) {
  const int N = 16;                                        // points per curve (was 8): rounder outline
  Pt loc[4][4], pts[4 * N]; int n = 0;
  float hw = e.w / 2, kx = e.kx, ky = e.ky;
  Pt L = { -hw, e.yl }, T = { 0, e.yt }, R = { hw, e.yl }, B = { 0, e.yb };
  Pt segs[4][4] = {
    { L, { -hw, e.yl - ky * (e.yl - e.yt) }, { -kx * hw, e.yt }, T },
    { T, { kx * hw, e.yt }, { hw, e.yl - ky * (e.yl - e.yt) }, R },
    { R, { hw, e.yl + ky * (e.yb - e.yl) }, { kx * hw, e.yb }, B },
    { B, { -kx * hw, e.yb }, { -hw, e.yl + ky * (e.yb - e.yl) }, L } };
  (void)loc;
  for (int s = 0; s < 4; s++)
    for (int i = 0; i < N; i++) {
      Pt q = bez(segs[s][0], segs[s][1], segs[s][2], segs[s][3], (float)i / N);
      pts[n++] = toScreen(q.x, q.y, e, cxOff);
    }
  u8g2.setDrawColor(1);
  fillPoly(pts, n);
  if (e.hasPupil) {                                        // black pupil with sub-pixel position (no "jumping")
    Pt c = toScreen(e.px, e.py, e, cxOff);
    float rx = e.prx * e.scale * S, ry = e.pry * e.scale * S;
    if (rx >= 0.8f && ry >= 0.8f) {
      const int M = 24; Pt el[M];
      for (int i = 0; i < M; i++) { float t = 2 * PI_F * i / M; el[i] = { c.x + rx * cosf(t), c.y + ry * sinf(t) }; }
      u8g2.setDrawColor(0); fillPoly(el, M); u8g2.setDrawColor(1);
    }
  }
}

void drawSmile(float s, float w, float depth, float sw) {
  if (s < 0.03f) return;
  float hw = w * s / 2, dd = depth * s;
  int r = (int)lroundf(sw * (0.6f + 0.4f * s) * MS / 2); if (r < 1) r = 1;
  u8g2.setDrawColor(1);
  for (int i = 0; i <= 32; i++) {                          // continuous stroke, uniform thickness
    float t = i / 32.0f;
    u8g2.drawDisc((int)lroundf(OX + (-hw + 2 * hw * t) * MS), (int)lroundf(MOUTH_Y + 4 * dd * t * (1 - t) * MS), r);
  }
}
void drawTilde(float s, float phase) {
  if (s < 0.03f) return;
  u8g2.setDrawColor(1);
  for (int i = 0; i <= 32; i++) {
    float t = i / 32.0f;
    int x = (int)lroundf(OX + (t - 0.5f) * 38 * s * MS);
    int y = (int)lroundf(MOUTH_Y + (2 + 4.5f * s * sinf(2 * PI_F * t + phase)) * MS);
    u8g2.drawDisc(x, y, 1);
  }
}
void drawZ(float x, float y, float sc) {                  // small vector Z
  float k = S * sc, X = OX + x * S, Y = OY + y * S;
  int x0 = (int)lroundf(X), y0 = (int)lroundf(Y), x1 = (int)lroundf(X + 16 * k), y1 = (int)lroundf(Y + 20 * k);
  u8g2.setDrawColor(1);
  for (int o = 0; o < 2; o++) {
    u8g2.drawLine(x0, y0 + o, x1, y0 + o);
    u8g2.drawLine(x1, y0, x0, y1);
    u8g2.drawLine(x1, y0 + o, x0, y1 + o);
    u8g2.drawLine(x0, y1 + o, x1, y1 + o);
  }
}

// ----- Per-frame parameters (identical to the generator) -----
const float AW_O[8]  = { 1.0f, 0.92f, 0.62f, 0.24f, 0.0f, 0.32f, 0.74f, 0.96f };
const float HP_P[8]  = { 0.0f, 0.22f, 0.50f, 0.82f, 1.0f, 0.72f, 0.40f, 0.12f };
const float HP_S[8]  = { 0.0f, 0.0f, 0.45f, 0.85f, 1.0f, 0.80f, 0.35f, 0.0f };
const float DZ_E[8]  = { 0.10f, 0.78f, 1.00f, 0.86f, 0.66f, 0.46f, 0.26f, 0.08f };
const float CU_X[8]  = { 0, -17.6f, -22, 13.2f, 22, 4.4f, 0, 0 };
const float CU_Y[8]  = { 0, 0, 0, 0, 0, -12.8f, -16, -3.2f };

Eye awakeEye(float o) {
  float h = lerpf(7, EYE_H, o), c = (1 - o) * 6;
  Eye e = makeEye(); e.w = EYE_W * (1 + 0.05f * (1 - o)); e.yl = c; e.yt = c - h / 2; e.yb = c + h / 2;
  setPupilFit(e, 0, 0, PRX, PRY, 1); return e;
}
Eye happyEye(float p) {
  float pb = clamp01(p * 1.3f);
  Eye e = makeEye();
  e.yt = lerpf(-50, -26, p); e.yb = lerpf(50, -9, pb); e.yl = lerpf(0, 14, p);
  e.w = lerpf(EYE_W, 92, p); e.kx = lerpf(0.66f, 0.72f, p); e.ky = lerpf(0.66f, 0.55f, p); e.dy = 6 * p;
  setPupilFit(e, 0, 0, PRX, PRY, smoothf(1 - (p - 0.2f) / 0.4f)); return e;
}

void renderFrame(uint8_t anim, uint8_t f) {
  u8g2.clearBuffer();
  Eye L = makeEye(), R = makeEye();
  switch (anim) {
    case ANIM_AWAKE:
      L = R = awakeEye(AW_O[f]); break;
    case ANIM_HAPPY:
      L = R = happyEye(HP_P[f]); break;
    case ANIM_DIZZY: {                       // f 0-7: intro/outro, f 8-15: full dizzy (loop)
      float e = (f >= 8) ? 1.0f : DZ_E[f];
      uint8_t fa = f & 7;
      float a = 2 * PI_F * fa / 8, Rr = 24 * e;
      float thL = (35 + 115 * fa) * PI_F / 180, thR = (200 - 140 * fa) * PI_F / 180;
      L.rot = 11 * e * sinf(a + 0.6f);  L.scale = 1 + 0.07f * e * sinf(a + 1.9f);
      R.rot = -11 * e * sinf(a + 2.3f); R.scale = 1 - 0.07f * e * sinf(a + 0.4f);
      L.hasPupil = R.hasPupil = true; L.prx = R.prx = 15; L.pry = R.pry = 18;
      L.px = Rr * cosf(thL); L.py = Rr * sinf(thL); R.px = Rr * cosf(thR); R.py = Rr * sinf(thR);
      break; }
    case ANIM_SLEEPING: {
      float b = 0.5f * (1 - cosf(2 * PI_F * f / 8)), th = 7 + 5 * b, d = 9 * b, c = 6 - 2 * b;
      L.w = EYE_W * 1.05f; L.yl = c - d / 2; L.yt = c + d / 2 - th / 2; L.yb = c + d / 2 + th / 2; R = L; break; }
    case ANIM_CURIOUS:
      L.hasPupil = R.hasPupil = true; L.prx = R.prx = PRX; L.pry = R.pry = PRY;
      L.px = R.px = CU_X[f]; L.py = R.py = CU_Y[f]; break;
  }
  drawEye(L, -EYE_DX); drawEye(R, EYE_DX);
  // Mouth always visible: base smile that grows into the big smile in HAPPY
  if (anim == ANIM_AWAKE || anim == ANIM_CURIOUS) drawSmile(1.0f, 40, 9, 7);
  if (anim == ANIM_HAPPY) { float s = HP_S[f]; drawSmile(1.0f, lerpf(40, 46, s), lerpf(9, 12, s), lerpf(7, 8, s)); }
  if (anim == ANIM_DIZZY) {
    float e = (f >= 8) ? 1.0f : DZ_E[f];
    drawTilde(fmaxf(0.6f, clamp01((e - 0.12f) / 0.5f)), 2 * PI_F * (f & 7) / 8);
  }
  if (anim == ANIM_SLEEPING) {
    float b = 0.5f * (1 - cosf(2 * PI_F * f / 8));
    drawSmile(0.6f, 40, 9, 7);                                        // sleepy little mouth
    drawZ(110, -22 - 6 * b, 0.60f + 0.08f * b);                      // small z
    drawZ(132, -46 - 6 * b, 0.90f + 0.12f * b);                      // big Z
  }
  u8g2.sendBuffer();
}

// ========================= BMI160 (I2C accelerometer) =========================
uint8_t bmiAddr = 0x68;
bool bmiOK = false;
bool bmiWrite(uint8_t reg, uint8_t v) { Wire.beginTransmission(bmiAddr); Wire.write(reg); Wire.write(v); return Wire.endTransmission() == 0; }
bool bmiRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(bmiAddr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)bmiAddr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
void bmiBegin() {                                        // setup() only
  uint8_t id = 0;
  for (uint8_t a = 0x68; a <= 0x69; a++) {
    bmiAddr = a;
    if (bmiRead(0x00, &id, 1) && id == 0xD1) { bmiOK = true; break; }
  }
  if (!bmiOK) return;
  bmiWrite(0x7E, 0xB6); delay(100);                      // reset
  bmiWrite(0x7E, 0x11); delay(60);                       // accelerometer normal mode
  bmiWrite(0x41, 0x05);                                  // range +/-4 g (8192 LSB/g)
  bmiWrite(0x40, 0x28);                                  // 100 Hz
}
uint32_t lastMotion = 0;                                 // last time it moved (even gently)
bool shakeDetected(uint32_t now) {
  static uint32_t lastPoll = 0, lastJolt = 0, winStart = 0, lastShake = 0;
  static uint8_t n = 0; static bool has = false;
  if (!bmiOK || now - lastPoll < 10) return false;       // read every 10 ms, non-blocking
  lastPoll = now;
  uint8_t b[6];
  if (!bmiRead(0x12, b, 6)) return false;
  float x = (int16_t)((b[1] << 8) | b[0]), y = (int16_t)((b[3] << 8) | b[2]), z = (int16_t)((b[5] << 8) | b[4]);
  float g = sqrtf(x * x + y * y + z * z) / 8192.0f;
  float dev = fabsf(g - 1.0f);
  if (dev > MOTION_G) lastMotion = now;
  if (dev > SHAKE_G && now - lastJolt > SHAKE_JOLT_GAP_MS) {
    lastJolt = now;
    if (n == 0 || now - winStart > SHAKE_WINDOW_MS) { winStart = now; n = 0; }
    if (++n >= SHAKE_JOLTS) {
      n = 0;
      if (!has || now - lastShake > SHAKE_COOLDOWN_MS) { has = true; lastShake = now; return true; }
    }
  }
  return false;
}
bool touchPressed(uint32_t now) {
  static bool last = false; static uint32_t t0 = 0;
  bool cur = digitalRead(PIN_TOUCH) == HIGH, ev = false;
  if (cur && !last && now - t0 > TOUCH_DEBOUNCE_MS) { ev = true; t0 = now; }
  last = cur; return ev;
}

// ========================= State machine (queue + millis) =========================
const uint8_t POSE_FRAME[4] = { 0, 2, 4, 6 };

const uint8_t QN = 16;
Step q[QN];
uint8_t qHead = 0, qTail = 0, qCount = 0;
void qClear() { qHead = qTail = qCount = 0; }
void qPush(uint8_t a, uint8_t f, uint16_t ms) { if (qCount < QN) { q[qTail] = { a, f, ms }; qTail = (qTail + 1) % QN; qCount++; } }
bool qPop(Step &s) { if (!qCount) return false; s = q[qHead]; qHead = (qHead + 1) % QN; qCount--; return true; }
uint16_t dur(uint8_t a, uint8_t f) { return random(T_MIN[a][f], (long)T_MAX[a][f] + 1); }
void pushFrame(uint8_t a, uint8_t f) { qPush(a, f, dur(a, f)); }
void pushSeq(uint8_t a, uint8_t f0, uint8_t f1) { for (uint8_t f = f0; f <= f1; f++) pushFrame(a, f); }

State state = ST_AWAKE;
Pose pose = POSE_C;
uint8_t blinksLeft = 2, shiftsLeft = 0;
uint32_t lastInteraction = 0, stepEnd = 0, dizzyStart = 0;
bool stepActive = false;
bool dizzyEnding = false;                                // true while playing the dizzy outro

void showFrame(uint8_t anim, uint8_t frame) {
  renderFrame(anim, frame);
#ifdef DEBUG_SERIAL
  Serial.print(ANIM_NAMES[anim]); Serial.print(' '); Serial.println(frame + 1);
#endif
}

void enterAwake() { state = ST_AWAKE; blinksLeft = random(1, 3); }
void enterHappy(uint32_t now) { qClear(); stepActive = false; state = ST_HAPPY; lastInteraction = now; pushSeq(ANIM_HAPPY, 0, 7); }
void enterDizzy(uint32_t now) {   // intro; refill() decides the loop and outro
  qClear(); stepActive = false; state = ST_DIZZY; dizzyEnding = false;
  lastInteraction = now; dizzyStart = now; lastMotion = now;
  pushSeq(ANIM_DIZZY, 0, 2);
}
void enterDrowsy() {   // slow eye close before sleeping
  qClear(); stepActive = false; state = ST_DROWSY;
  qPush(ANIM_AWAKE, 1, 220); qPush(ANIM_AWAKE, 2, 320); qPush(ANIM_AWAKE, 3, 420); qPush(ANIM_AWAKE, 4, 520);
}
int8_t transitFrame(Pose from, Pose to) {
  if (from == POSE_C && to == POSE_L) return 1;
  if (from == POSE_L && to == POSE_R) return 3;
  if (from == POSE_R && to == POSE_U) return 5;
  if (from == POSE_U && to == POSE_C) return 7;
  return -1;
}

void refill() {   // queue is empty: decide what comes next
  switch (state) {
    case ST_AWAKE:
      pushFrame(ANIM_AWAKE, 0);                                   // eyes open 2-5 s
      if (blinksLeft > 0) { pushSeq(ANIM_AWAKE, 1, 7); blinksLeft--; }   // 220 ms blink
      else {
        pushSeq(ANIM_AWAKE, 1, 6);                                // blink and switch to CURIOUS
        state = ST_CURIOUS; shiftsLeft = random(3, 6); pose = POSE_C;
        pushFrame(ANIM_CURIOUS, 0);
      }
      break;
    case ST_CURIOUS:
      if (shiftsLeft > 0) {
        Pose next = (random(100) < 45) ? (Pose)((pose + 1) % 4) : (Pose)random(0, 4);
        if (next == pose) next = (Pose)((pose + 1) % 4);          // never repeats the same gaze
        int8_t t = transitFrame(pose, next);
        if (t >= 0) pushFrame(ANIM_CURIOUS, t);
        pushFrame(ANIM_CURIOUS, POSE_FRAME[next]);
        pose = next; shiftsLeft--;
      } else { pushSeq(ANIM_AWAKE, 3, 7); enterAwake(); }          // back to AWAKE with a blink
      break;
    case ST_DIZZY:
      if (!dizzyEnding) {
        if (millis() - lastMotion < DIZZY_HOLD_MS) {               // still moving or stopped recently
          for (uint8_t f = 8; f < 16; f++) qPush(ANIM_DIZZY, f, 110);   // one full-dizzy loop
        } else {
          dizzyEnding = true; pushSeq(ANIM_DIZZY, 3, 7);           // delay is over: wind down
        }
        break;
      }
      dizzyEnding = false;                                         // outro done: back to AWAKE
      lastInteraction = millis();
      enterAwake(); refill(); break;
    case ST_HAPPY:
      lastInteraction = millis();                                  // idle time counts from here
      enterAwake(); refill(); break;
    case ST_DROWSY:
      state = ST_SLEEPING; refill(); break;
    case ST_SLEEPING:
      pushSeq(ANIM_SLEEPING, 0, 7); break;                         // breathing every 3-3.7 s
  }
}

void setup() {
#ifdef DEBUG_SERIAL
  Serial.begin(115200);
#endif
  pinMode(PIN_TOUCH, INPUT);
  Wire.setPins(PIN_SDA, PIN_SCL);   // I2C on D4/D5 even if a generic board is selected
  u8g2.begin();
  u8g2.setBusClock(800000);        // if the OLED shows garbage, lower to 400000
  bmiBegin();
  randomSeed(esp_random());
  lastInteraction = millis();
  enterAwake();
}

void loop() {
  uint32_t now = millis();
  bool shake = shakeDetected(now);
  bool touch = touchPressed(now);

  if (now - lastMotion < 50) lastInteraction = now; // any motion counts as activity (won't sleep)

  if (shake) {                                   // shake has priority
    if (state != ST_DIZZY || dizzyEnding) enterDizzy(now);
    else lastInteraction = now;                  // already dizzy: keep looping while it moves
  } else if (touch) {                            // touch: interrupts sleep and curiosity
    if (state == ST_HAPPY || state == ST_DIZZY) lastInteraction = now;
    else enterHappy(now);
  }

  if ((state == ST_AWAKE || state == ST_CURIOUS) && now - lastInteraction >= IDLE_SLEEP_MS)
    enterDrowsy();

  if (!stepActive || (int32_t)(now - stepEnd) >= 0) {
    if (!qCount) refill();
    Step s;
    if (qPop(s)) { showFrame(s.anim, s.frame); stepEnd = now + s.ms; stepActive = true; }
  }
}
