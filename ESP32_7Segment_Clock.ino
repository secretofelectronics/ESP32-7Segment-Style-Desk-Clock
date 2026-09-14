/*
  =========================================================================
  Realistic 7-Segment Digital Clock  —  ESP32 + ILI9341 (SPI, non-touch)
  =========================================================================
  Features
  --------
  - Big, realistic hexagonal 7-segment digits (pointed tips, like real LEDs)
  - Dim "ghost" segments for the unlit parts, like a real display
  - Smooth fade animation when a digit changes
  - Hour:Minute only, 12-hour format with AM/PM
  - No RTC module: time comes from NTP over WiFi
  - Timezone is detected automatically from your public IP (no manual UTC
    offset entry) using https://worldtimeapi.org
  - Auto WiFi reconnect + status indicator dot
  - Blinking colon (since seconds aren't shown, it indicates the clock is
    alive)
  - Power-on "lamp test" (classic 88:88 flash) then fades into real time
  - NEW: one push-button cycles through 6 modern digit-color themes
    (Cyan, Electric Blue, Mint Green, Amber Gold, Hot Pink, Classic Red).
    The chosen theme is remembered across power cycles (stored in flash).

  Libraries required (install via Library Manager)
  --------------------------------------------------
  - Adafruit GFX Library
  - Adafruit ILI9341
  - ArduinoJson (by Benoit Blanchon), version 6.x
  - ESP32 board package (provides WiFi, WiFiClientSecure, HTTPClient,
    Preferences, time.h)

  Wiring (edit the pins below to match your board)
  --------------------------------------------------
  ILI9341        ESP32 (VSPI default)
  ---------      --------------------
  VCC        ->  3.3V
  GND        ->  GND
  CS         ->  GPIO 5
  RESET      ->  GPIO 4
  DC/RS      ->  GPIO 2
  SDI(MOSI)  ->  GPIO 23
  SCK        ->  GPIO 18
  LED        ->  3.3V (or a PWM pin if you want brightness control)
  SDO(MISO)  ->  GPIO 19 (not required for this project)

  Color-change button
  --------------------
  One momentary push-button between GPIO 15 and GND (internal pull-up is
  used, so no external resistor is needed). Each press advances to the
  next color theme.
  =========================================================================
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ----------------------------------------------------------------------
// USER CONFIG
// ----------------------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID"; //your wifi ssid
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD"; //your wifi password

// Used ONLY if both automatic timezone lookups fail (e.g. no internet route
// to the lookup APIs, or they're temporarily down). Set this to your own
// region's UTC offset in seconds as a safety net so the clock is never
// wildly wrong. India Standard Time = UTC+5:30 = 19800 seconds.
const long FALLBACK_GMT_OFFSET_SEC = 19800; // change if you're outside India

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4

#define BUTTON_PIN 15   // color-change button, other leg to GND

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;

// ----------------------------------------------------------------------
// FIXED UI COLORS (not part of the theme)
// ----------------------------------------------------------------------
#define COLOR_BG      0x0000   // black background
#define COLOR_TEXT    0x7BEF   // light grey  - status text
#define COLOR_ACCENT  0x07E0   // green       - wifi/time OK
#define COLOR_WARN    0xF800   // red         - wifi/time problem
#define COLOR_DIMTXT  0x39C7   // dim grey    - small footer label

// ----------------------------------------------------------------------
// DIGIT COLOR THEMES  (modern, light/attractive palette + a classic red)
// Off/ghost color is auto-derived as a dim version of the on-color, so
// every theme automatically gets a matching "unlit LED" look.
// ----------------------------------------------------------------------
struct ColorTheme { const char* name; uint8_t r, g, b; };

// How visible the "unlit" segments are, as a fraction of the on-color's
// brightness. Lower = more invisible/subtle. 0.0 = fully invisible (pure
// black, matches the background exactly). Try 0.02-0.05 for a barely-there
// ghost look, or 0.0 for completely hidden inactive segments.
const float OFF_SEGMENT_BRIGHTNESS = 0.03;

const ColorTheme THEMES[] = {
  {"Cyan",          0, 229, 255},
  {"Electric Blue", 64, 140, 255},
  {"Mint Green",    60, 255, 170},
  {"Amber Gold",   255, 180,  40},
  {"Hot Pink",     255,  60, 170},
  {"Classic Red",  248,  30,  30},
};
const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

int themeIndex = 0;
uint16_t COLOR_ON, COLOR_OFF; // computed from the active theme

void applyTheme(int idx) {
  themeIndex = ((idx % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
  ColorTheme t = THEMES[themeIndex];
  COLOR_ON  = tft.color565(t.r, t.g, t.b);
  COLOR_OFF = tft.color565((uint8_t)(t.r * OFF_SEGMENT_BRIGHTNESS), (uint8_t)(t.g * OFF_SEGMENT_BRIGHTNESS), (uint8_t)(t.b * OFF_SEGMENT_BRIGHTNESS));
}

// ----------------------------------------------------------------------
// LAYOUT
// ----------------------------------------------------------------------
struct DigitPos { int16_t x, y, w, h; };

const int16_t DIGIT_W = 54;
const int16_t DIGIT_H = 118;
const int16_t THICK   = 12;
const int16_t GAP     = 10;
const int16_t COLON_W = 26;

DigitPos digitPos[4];
int16_t colonX, colonY;

// Manual forward declaration: Arduino's auto-prototype generator inserts
// prototypes near the very top of the file (before DigitPos is defined),
// which breaks for any function that takes DigitPos as a parameter.
// Declaring it ourselves here (after DigitPos exists) prevents that.
void drawSegmentAt(DigitPos p, int segIndex, uint16_t color);

// ----------------------------------------------------------------------
// SEGMENT DATA
// Segment index: 0=A(top) 1=B(top-right) 2=C(bottom-right) 3=D(bottom)
//                4=E(bottom-left) 5=F(top-left) 6=G(middle)
// Row 10 = BLANK (all segments off) -> used for a suppressed leading zero
// ----------------------------------------------------------------------
const bool DIGIT_SEG[11][7] = {
  {1,1,1,1,1,1,0}, // 0
  {0,1,1,0,0,0,0}, // 1
  {1,1,0,1,1,0,1}, // 2
  {1,1,1,1,0,0,1}, // 3
  {0,1,1,0,0,1,1}, // 4
  {1,0,1,1,0,1,1}, // 5
  {1,0,1,1,1,1,1}, // 6
  {1,1,1,0,0,0,0}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}, // 9
  {0,0,0,0,0,0,0}, // 10 = blank
};
const int BLANK_DIGIT = 10;

int currentDigit[4] = {-1,-1,-1,-1}; // -1 = not drawn yet
bool timeSynced = false;
long gmtOffsetSec = 0;

unsigned long lastColonBlink = 0;
bool colonOn = true;
unsigned long lastWifiCheck = 0;

int lastPMState = -1; // -1 unknown, 0 AM, 1 PM

unsigned long footerRevertAt = 0; // 0 = no pending revert

// =========================================================================
//  LOW-LEVEL SEGMENT DRAWING (hexagonal / pointed-tip shapes, like real LEDs)
// =========================================================================

void drawHSeg(int16_t cx, int16_t cy, int16_t len, int16_t thick, uint16_t color) {
  int16_t half = thick / 2;
  int16_t tip  = half;
  int16_t x1 = cx - len / 2;
  int16_t x2 = cx + len / 2;
  tft.fillRect(x1 + tip, cy - half, (x2 - tip) - (x1 + tip), thick, color);
  tft.fillTriangle(x1, cy, x1 + tip, cy - half, x1 + tip, cy + half, color);
  tft.fillTriangle(x2, cy, x2 - tip, cy - half, x2 - tip, cy + half, color);
}

void drawVSeg(int16_t cx, int16_t cy, int16_t len, int16_t thick, uint16_t color) {
  int16_t half = thick / 2;
  int16_t tip  = half;
  int16_t y1 = cy - len / 2;
  int16_t y2 = cy + len / 2;
  tft.fillRect(cx - half, y1 + tip, thick, (y2 - tip) - (y1 + tip), color);
  tft.fillTriangle(cx, y1, cx - half, y1 + tip, cx + half, y1 + tip, color);
  tft.fillTriangle(cx, y2, cx - half, y2 - tip, cx + half, y2 - tip, color);
}

void drawSegmentAt(DigitPos p, int segIndex, uint16_t color) {
  int16_t midY    = p.y + p.h / 2;
  int16_t segLenH = p.w - THICK;
  int16_t segLenV = (p.h - 3 * THICK) / 2;
  int16_t cxL     = p.x + THICK / 2;
  int16_t cxR     = p.x + p.w - THICK / 2;
  int16_t cxMid   = p.x + p.w / 2;

  switch (segIndex) {
    case 0: drawHSeg(cxMid, p.y + THICK / 2, segLenH, THICK, color); break;                       // A
    case 1: drawVSeg(cxR, p.y + THICK + segLenV / 2, segLenV, THICK, color); break;                // B
    case 2: drawVSeg(cxR, p.y + p.h - THICK - segLenV / 2, segLenV, THICK, color); break;          // C
    case 3: drawHSeg(cxMid, p.y + p.h - THICK / 2, segLenH, THICK, color); break;                  // D
    case 4: drawVSeg(cxL, p.y + p.h - THICK - segLenV / 2, segLenV, THICK, color); break;          // E
    case 5: drawVSeg(cxL, p.y + THICK + segLenV / 2, segLenV, THICK, color); break;                // F
    case 6: drawHSeg(cxMid, midY, segLenH, THICK, color); break;                                   // G
  }
}

void drawDigitInstant(int idx, int value) {
  DigitPos p = digitPos[idx];
  for (int s = 0; s < 7; s++) {
    uint16_t col = DIGIT_SEG[value][s] ? COLOR_ON : COLOR_OFF;
    drawSegmentAt(p, s, col);
  }
  currentDigit[idx] = value;
}

uint16_t lerpColor(uint16_t c1, uint16_t c2, float t) {
  uint8_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint8_t r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
  uint8_t r = r1 + (int)((r2 - r1) * t);
  uint8_t g = g1 + (int)((g2 - g1) * t);
  uint8_t b = b1 + (int)((b2 - b1) * t);
  return (r << 11) | (g << 5) | b;
}

// Smoothly fades only the segments that actually change (real LED style)
void updateDigitAnimated(int idx, int newValue) {
  if (currentDigit[idx] == newValue) return;

  int oldValue = (currentDigit[idx] < 0) ? BLANK_DIGIT : currentDigit[idx];
  DigitPos p = digitPos[idx];
  const int STEPS = 10;

  for (int s = 1; s <= STEPS; s++) {
    float frac = (float)s / STEPS;
    for (int seg = 0; seg < 7; seg++) {
      bool wasOn = DIGIT_SEG[oldValue][seg];
      bool isOn  = DIGIT_SEG[newValue][seg];
      if (wasOn == isOn) continue; // unchanged segment, skip redraw
      uint16_t col = isOn ? lerpColor(COLOR_OFF, COLOR_ON, frac)
                          : lerpColor(COLOR_ON, COLOR_OFF, frac);
      drawSegmentAt(p, seg, col);
    }
    delay(10);
  }
  // snap to exact final colors
  for (int seg = 0; seg < 7; seg++) {
    uint16_t col = DIGIT_SEG[newValue][seg] ? COLOR_ON : COLOR_OFF;
    drawSegmentAt(p, seg, col);
  }
  currentDigit[idx] = newValue;
}

void lampTest() {
  for (int i = 0; i < 4; i++) drawDigitInstant(i, 8); // 88:88
  drawColonColor(COLOR_ON);
  delay(700);
  for (int i = 0; i < 4; i++) currentDigit[i] = -1; // force fade-in on next real update
}

// =========================================================================
//  LAYOUT / COLON / LABELS
// =========================================================================

void computeLayout() {
  int totalWidth = DIGIT_W * 4 + COLON_W + GAP * 4;
  int startX = (tft.width() - totalWidth) / 2;
  int y = (tft.height() - DIGIT_H) / 2 + 10;

  digitPos[0] = {(int16_t)startX, (int16_t)y, DIGIT_W, DIGIT_H};
  digitPos[1] = {(int16_t)(digitPos[0].x + DIGIT_W + GAP), (int16_t)y, DIGIT_W, DIGIT_H};
  colonX = digitPos[1].x + DIGIT_W + GAP;
  colonY = y;
  digitPos[2] = {(int16_t)(colonX + COLON_W + GAP), (int16_t)y, DIGIT_W, DIGIT_H};
  digitPos[3] = {(int16_t)(digitPos[2].x + DIGIT_W + GAP), (int16_t)y, DIGIT_W, DIGIT_H};
}

void drawColonColor(uint16_t color) {
  int16_t cx = colonX + COLON_W / 2;
  int16_t r  = THICK / 2;
  int16_t cy1 = colonY + DIGIT_H / 3;
  int16_t cy2 = colonY + (DIGIT_H * 2) / 3;
  tft.fillCircle(cx, cy1, r, color);
  tft.fillCircle(cx, cy2, r, color);
}

void drawStatusDot(bool ok) {
  tft.fillCircle(12, 12, 5, ok ? COLOR_ACCENT : COLOR_WARN);
}

void drawAmPm(bool isPM, bool force) {
  int pmVal = isPM ? 1 : 0;
  if (!force && lastPMState == pmVal) return;
  lastPMState = pmVal;
  int16_t x = tft.width() - 55;
  int16_t y = 6;
  tft.fillRect(x - 4, y - 2, 58, 20, COLOR_BG);
  tft.setTextColor(COLOR_ON);
  tft.setTextSize(2);
  tft.setCursor(x, y);
  tft.print(isPM ? "PM" : "AM");
}

void drawFooter(const char* msg, uint16_t color) {
  tft.fillRect(0, tft.height() - 18, tft.width(), 18, COLOR_BG);
  tft.setTextColor(color);
  tft.setTextSize(1);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, tft.height() - 14);
  tft.print(msg);
}

void showNormalFooter() {
  drawFooter(timeSynced ? "12-HOUR  |  NTP SYNCED" : "12-HOUR  |  NO SYNC", COLOR_DIMTXT);
}

void bootMessage(const char* msg, int line) {
  // Clear only this line's row before printing, so successive status
  // messages on the same line never overlap or smear into each other.
  tft.fillRect(0, 90 + line * 24, tft.width(), 22, COLOR_BG);
  tft.setTextColor(COLOR_TEXT);
  tft.setTextSize(2);
  tft.setCursor(20, 90 + line * 24);
  tft.print(msg);
}

// Redraw everything currently on screen in the active theme's colors
// (used right after a color-theme change, no animation needed here).
void redrawStaticUI() {
  for (int i = 0; i < 4; i++) {
    int v = (currentDigit[i] < 0) ? BLANK_DIGIT : currentDigit[i];
    drawDigitInstant(i, v);
  }
  drawColonColor(colonOn ? COLOR_ON : COLOR_OFF);
  drawAmPm(lastPMState == 1, true);
  drawFooter(THEMES[themeIndex].name, COLOR_ON);
  footerRevertAt = millis() + 1500; // show the theme name briefly, then revert
}

// =========================================================================
//  COLOR-CHANGE BUTTON
// =========================================================================

unsigned long lastButtonPress = 0;
int lastButtonState = HIGH;

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading == LOW && lastButtonState == HIGH && millis() - lastButtonPress > 250) {
    lastButtonPress = millis();
    applyTheme(themeIndex + 1);
    prefs.putInt("theme", themeIndex);
    redrawStaticUI();
  }
  lastButtonState = reading;
}

// =========================================================================
//  WIFI + TIMEZONE + NTP
// =========================================================================

void connectWiFi() {
  tft.fillScreen(COLOR_BG);
  bootMessage("Connecting WiFi", 0);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  int dotCount = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    tft.print(".");
    dotCount++;
    if (dotCount % 20 == 0) {
      tft.fillRect(0, 90, tft.width(), 24, COLOR_BG);
      tft.setCursor(20, 90);
    }
  }
}

// Primary provider: worldtimeapi.org (HTTPS). Detects offset (raw + DST)
// from the device's public IP.
bool fetchTimezoneOffset_WorldTimeAPI(long &offsetOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // skip TLS cert validation (fine for this public read-only API)
  HTTPClient https;

  if (!https.begin(client, "https://worldtimeapi.org/api/ip")) {
    Serial.println("[TZ] worldtimeapi: begin() failed");
    return false;
  }
  int code = https.GET();
  Serial.printf("[TZ] worldtimeapi HTTP code: %d\n", code);
  if (code != 200) {
    https.end();
    return false;
  }
  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(1536);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[TZ] worldtimeapi JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  long rawOffset = doc["raw_offset"] | 0;
  long dstOffset = doc["dst_offset"] | 0;
  offsetOut = rawOffset + dstOffset;
  Serial.printf("[TZ] worldtimeapi offset: %ld sec\n", offsetOut);
  return true;
}

// Backup provider: ip-api.com (plain HTTP, no TLS needed -> avoids
// certificate/handshake failures). Returns the UTC offset (DST-adjusted)
// directly in the "offset" field.
bool fetchTimezoneOffset_IpApi(long &offsetOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  if (!http.begin("http://ip-api.com/json/?fields=status,offset,timezone")) {
    Serial.println("[TZ] ip-api: begin() failed");
    return false;
  }
  int code = http.GET();
  Serial.printf("[TZ] ip-api HTTP code: %d\n", code);
  if (code != 200) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[TZ] ip-api JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  const char* status = doc["status"] | "";
  if (String(status) != "success") {
    Serial.println("[TZ] ip-api status != success");
    return false;
  }
  offsetOut = doc["offset"] | 0;
  Serial.printf("[TZ] ip-api offset: %ld sec (%s)\n", offsetOut, (const char*)(doc["timezone"] | ""));
  return true;
}

void setupTimeFromNTP() {
  bootMessage("Detecting timezone...", 1);
  long offset = 0;
  bool ok = fetchTimezoneOffset_WorldTimeAPI(offset);

  if (!ok) {
    bootMessage("Trying backup TZ source...", 1);
    ok = fetchTimezoneOffset_IpApi(offset);
  }

  if (ok) {
    gmtOffsetSec = offset;
  } else {
    // Both lookups failed (e.g. blocked/unreachable APIs) - use the
    // hardcoded safety-net offset instead of silently defaulting to UTC.
    gmtOffsetSec = FALLBACK_GMT_OFFSET_SEC;
    Serial.println("[TZ] Both lookups failed, using FALLBACK_GMT_OFFSET_SEC");
    bootMessage("TZ lookup failed, using fallback", 1);
  }

  // daylightOffset is 0 because both providers already fold DST into the offset above
  configTime(gmtOffsetSec, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  bootMessage("Syncing time (NTP)", 2);
  struct tm timeinfo;
  unsigned long start = millis();
  while (!getLocalTime(&timeinfo, 500) && millis() - start < 15000) {
    tft.print(".");
  }
  timeSynced = getLocalTime(&timeinfo);

  if (timeSynced) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("[TIME] Synced local time: %s (offset %ld sec)\n", buf, gmtOffsetSec);
  } else {
    Serial.println("[TIME] NTP sync failed within timeout");
  }
}

// =========================================================================
//  SETUP / LOOP
// =========================================================================

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  tft.begin();
  tft.setRotation(1); // landscape 320x240 (use 3 to flip upside down)
  tft.fillScreen(COLOR_BG);

  prefs.begin("clockcfg", false);
  int savedTheme = prefs.getInt("theme", 0);
  applyTheme(savedTheme);

  computeLayout();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupTimeFromNTP();
  } else {
    bootMessage("WiFi failed - check credentials", 1);
    delay(2000);
  }

  tft.fillScreen(COLOR_BG);
  lampTest();
  drawStatusDot(WiFi.status() == WL_CONNECTED && timeSynced);
  showNormalFooter();
}

void updateHourTensDigit(int h1) {
  // Always show the digit, including a leading "0" for hours 1-9 (e.g. "01:14")
  updateDigitAnimated(0, h1);
}

void loop() {
  handleButton();

  // --- WiFi watchdog / reconnect ---
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      drawStatusDot(false);
      WiFi.reconnect();
    } else {
      drawStatusDot(true);
    }
  }

  // --- Update clock digits ---
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) {
    int hour24 = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;
    bool isPM = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    int h1 = hour12 / 10;
    int h2 = hour12 % 10;
    int m1 = minute / 10;
    int m2 = minute % 10;

    updateHourTensDigit(h1);
    updateDigitAnimated(1, h2);
    updateDigitAnimated(2, m1);
    updateDigitAnimated(3, m2);
    drawAmPm(isPM, false);
  }

  // --- Blinking colon (indicates the clock is alive since seconds aren't shown) ---
  if (millis() - lastColonBlink > 500) {
    lastColonBlink = millis();
    colonOn = !colonOn;
    drawColonColor(colonOn ? COLOR_ON : COLOR_OFF);
  }

  // --- Revert footer back to normal status text after showing theme name ---
  if (footerRevertAt != 0 && millis() > footerRevertAt) {
    footerRevertAt = 0;
    showNormalFooter();
  }

  delay(20);
}
