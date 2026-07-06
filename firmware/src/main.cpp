#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include "board_config.h"

#if __has_include("dashboard_config.h")
#include "dashboard_config.h"
#else
#include "dashboard_config.example.h"
#endif

static constexpr uint16_t SCREEN_W = 320;
static constexpr uint16_t SCREEN_H = 480;

// Theme-dependent colors are mutable globals set by applyTheme(); accents below
// stay constant (saturated enough to read on any theme).
uint32_t COLOR_BG = 0x101215;
uint32_t COLOR_PANEL = 0x1C2128;
uint32_t COLOR_PANEL_2 = 0x242B33;
uint32_t COLOR_TEXT = 0xF3F6FA;
uint32_t COLOR_MUTED = 0x94A3B8;
uint32_t COLOR_RULE = 0x323A45;
uint32_t COLOR_CLOUD = 0xD7DEE8;
uint32_t COLOR_MOON = 0xE8EDF4;
static constexpr uint32_t COLOR_GOOD = 0x2DD4BF;
static constexpr uint32_t COLOR_WARN = 0xFBBF24;
static constexpr uint32_t COLOR_DANGER = 0xF87171;
static constexpr uint32_t COLOR_CLAUDE = 0xF97316;
static constexpr uint32_t COLOR_CODEX = 0x38BDF8;
static constexpr uint32_t COLOR_WEATHER = 0xFACC15;
static constexpr uint32_t COLOR_RAIN = 0x38BDF8;
static constexpr uint32_t COLOR_STAR = 0xFDE68A;

enum class Theme { Dark = 0, Black = 1, Light = 2 };
Theme uiTheme = Theme::Dark;

// UI language. Chinese strings must be drawn with the efontTW faces (set via
// the setUiFont* helpers below) — DejaVu/Font2 carry no CJK glyphs.
enum class Lang { EN = 0, ZH = 1 };
Lang uiLang = Lang::EN;
bool zhUi() { return uiLang == Lang::ZH; }
const char* tr(const char* en, const char* zh) { return zhUi() ? zh : en; }

void applyTheme(Theme t) {
  uiTheme = t;
  switch (t) {
    case Theme::Dark:
      COLOR_BG = 0x101215; COLOR_PANEL = 0x1C2128; COLOR_PANEL_2 = 0x242B33;
      COLOR_TEXT = 0xF3F6FA; COLOR_MUTED = 0x94A3B8; COLOR_RULE = 0x323A45;
      COLOR_CLOUD = 0xD7DEE8; COLOR_MOON = 0xE8EDF4;
      break;
    case Theme::Black:
      COLOR_BG = 0x000000; COLOR_PANEL = 0x0C0E11; COLOR_PANEL_2 = 0x191C21;
      COLOR_TEXT = 0xFFFFFF; COLOR_MUTED = 0x8A93A0; COLOR_RULE = 0x2A2E36;
      COLOR_CLOUD = 0xC7D0DC; COLOR_MOON = 0xE8EDF4;
      break;
    case Theme::Light:
      COLOR_BG = 0xEEF1F5; COLOR_PANEL = 0xFFFFFF; COLOR_PANEL_2 = 0xE6EAF0;
      COLOR_TEXT = 0x1B2430; COLOR_MUTED = 0x5B6675; COLOR_RULE = 0xCED6E0;
      COLOR_CLOUD = 0x9AA7B8; COLOR_MOON = 0x8794A6;
      break;
  }
}

static constexpr uint16_t SETUP_DNS_PORT = 53;
// Config-overridable defaults: dashboard_config.h may #define these to localise
// the build; otherwise the values below apply. See dashboard_config.example.h.
#ifndef DASHBOARD_SETUP_AP_SSID
#define DASHBOARD_SETUP_AP_SSID "ESP32-Dashboard-Setup"
#endif
#ifndef DASHBOARD_SETUP_AP_PASSWORD
#define DASHBOARD_SETUP_AP_PASSWORD "esp32setup"
#endif
static constexpr const char* SETUP_AP_SSID = DASHBOARD_SETUP_AP_SSID;
static constexpr const char* SETUP_AP_PASSWORD = DASHBOARD_SETUP_AP_PASSWORD;
static constexpr int SETUP_BACK_X = 34;
static constexpr int SETUP_BACK_Y = 354;
static constexpr int SETUP_BACK_W = 118;
static constexpr int SETUP_BACK_H = 42;
static constexpr unsigned long LONG_PRESS_MS = 1200;
// Redraw cadence for the live clock and gentle icon animation. The dashboard is
// double-buffered through a PSRAM sprite, so a full repaint at this rate is
// flicker-free and cheap.
static constexpr unsigned long RENDER_INTERVAL_MS = 1000;
// Attempt a Wi-Fi reconnect at most this often while disconnected.
static constexpr unsigned long RECONNECT_INTERVAL_MS = 15000;
static constexpr size_t DASHBOARD_MAX_PAYLOAD_BYTES = 24 * 1024;
static constexpr int DASHBOARD_FETCH_ATTEMPTS = 4;
static constexpr size_t MAX_SHORT_TEXT = 12;
static constexpr size_t MAX_LABEL_TEXT = 28;
static constexpr size_t MAX_RESET_TEXT = 16;
static constexpr size_t MAX_SOURCE_TEXT = 32;
// POSIX TZ string (default Taipei, UTC+8, no DST) and NTP servers for the
// clock. Override in dashboard_config.h to run the device in another timezone.
#ifndef DASHBOARD_CLOCK_TZ
#define DASHBOARD_CLOCK_TZ "CST-8"
#endif
#ifndef DASHBOARD_NTP_1
#define DASHBOARD_NTP_1 "pool.ntp.org"
#endif
#ifndef DASHBOARD_NTP_2
#define DASHBOARD_NTP_2 "time.google.com"
#endif
static constexpr const char* CLOCK_TZ = DASHBOARD_CLOCK_TZ;
static constexpr const char* NTP_SERVER_1 = DASHBOARD_NTP_1;
static constexpr const char* NTP_SERVER_2 = DASHBOARD_NTP_2;
static constexpr uint8_t BRIGHTNESS_DAY = 210;
static constexpr uint8_t BRIGHTNESS_NIGHT = 70;
static constexpr int NIGHT_START_HOUR = 23;
static constexpr int NIGHT_END_HOUR = 7;

enum class WeatherKind {
  Sunny,
  PartlyCloudy,
  Cloudy,
  Rain,
  Thunderstorm,
  Fog,
  Night,
  NightCloudy,
};

static constexpr int MAX_HOURLY = 6;

struct HourlyPoint {
  String hour;   // "10", "11" ... (local hour label)
  int tempC = 0;
  int rainPct = 0;
};

struct WeatherState {
  String city = DASHBOARD_CITY;
  String weekday = "Tue";
  String date = "Jun 30";
  String day = "30";
  String month = "Jun";
  String year = "2026";
  int tempC = 27;
  int rainPct = 40;
  float rainMm = 0.0f;
  bool isRaining = false;
  bool rainAlert = false;
  int aqi = -1;         // current US AQI, -1 = unknown
  int uv = -1;          // today's max UV index, -1 = unknown
  String sunrise = "";  // "05:08", empty = unknown
  String sunset = "";
  String cityZh = "";   // reverse-geocoded Chinese locality, empty = unknown
  WeatherKind kind = WeatherKind::PartlyCloudy;
  String label = "Partly cloudy";
  HourlyPoint hourly[MAX_HOURLY];
  int hourlyCount = 0;
};

struct QuotaState {
  String name;
  int usedPct = 0;
  bool known = true;
  String reset = "--";
  int64_t tokens = -1; // total tokens in this window, -1 = unknown
  int costTwd = -1;    // API-equivalent cost in NT$, -1 = unknown

  QuotaState() = default;
  QuotaState(const char* quotaName, int usedPercent, const char* resetAt)
      : name(quotaName), usedPct(usedPercent), reset(resetAt) {}
};

// Per-agent daily token history + accumulated totals for the detail chart.
static constexpr int DAILY_MAX = 14;
static constexpr int MODEL_MAX = 3;
struct ModelShare {
  String name;
  int pct = 0;      // share of the chart window's tokens, 0..100
  int64_t tok = 0;  // tokens in the chart window
};
struct AgentChart {
  long daily[DAILY_MAX] = {0};  // per-day tokens stay well under 32-bit
  int dailyCount = 0;
  int64_t totalTok = -1;        // all-time total can exceed 32-bit, -1 = unknown
  int totalTwd = -1;
  ModelShare models[MODEL_MAX];
  int modelCount = 0;
};

struct DashboardState {
  WeatherState weather;
  QuotaState claude5h{"5hr", 28, "14:30"};
  QuotaState claudeWeekly{"Weekly", 59, "Mon"};
  QuotaState codex5h{"5hr", 12, "15:10"};
  QuotaState codexWeekly{"Weekly", 37, "Mon"};
  AgentChart claudeChart;
  AgentChart codexChart;
  bool online = false;
  String source = "demo";
};

DashboardDisplay lcd;
// Off-screen back buffer in PSRAM. All dashboard/setup drawing goes through the
// `gfx` pointer, which points at `canvas` when the sprite allocates and falls
// back to drawing straight to `lcd` if PSRAM is exhausted.
LGFX_Sprite canvas(&lcd);
lgfx::LGFXBase* gfx = &lcd;
bool useCanvas = false;

// Language-aware font selection: English keeps the original faces so layouts
// stay pixel-identical; Chinese swaps in the closest efontTW size.
void setUiFont() { gfx->setFont(zhUi() ? (const lgfx::IFont*)&fonts::efontTW_16 : &fonts::Font2); }
void setUiFont12() { gfx->setFont(zhUi() ? (const lgfx::IFont*)&fonts::efontTW_12 : &fonts::DejaVu12); }
void setUiFont18() { gfx->setFont(zhUi() ? (const lgfx::IFont*)&fonts::efontTW_16 : &fonts::DejaVu18); }
void setUiFont24() { gfx->setFont(zhUi() ? (const lgfx::IFont*)&fonts::efontTW_24 : &fonts::DejaVu24); }
// For text drawn as Font2 + setTextSize(2): Chinese uses efontTW_24 at size 1
// instead of a pixel-doubled 16 so it stays crisp. Pair with resetUiBigFont().
void setUiBigFont() {
  if (zhUi()) {
    gfx->setFont(&fonts::efontTW_24);
    gfx->setTextSize(1);
  } else {
    gfx->setTextSize(2);
  }
}
void resetUiBigFont() {
  if (zhUi()) gfx->setFont(&fonts::Font2);
}

DashboardState state;
Preferences preferences;
WebServer setupServer(80);
DNSServer dnsServer;
String runtimeWifiSsid;
String runtimeWifiPassword;
String runtimeApiUrl;
unsigned long lastFetchMs = 0;
unsigned long lastRenderMs = 0;
unsigned long lastReconnectMs = 0;
unsigned long lastSuccessMs = 0;
unsigned long touchStartMs = 0;
// Advances once per render tick to drive blink/weather animation frames.
uint8_t animationFrame = 0;
uint8_t currentBrightness = BRIGHTNESS_DAY;
bool timeSynced = false;
bool setupMode = false;
bool touchWasDown = false;
bool longPressFired = false;
uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;

// Which touch page is showing. The legacy AP portal is still handled separately
// via setupMode; WifiScan/WifiKey are the on-device Wi-Fi entry flow.
enum class View {
  Dashboard, DetailCodex, DetailClaude, Weather, Settings, WifiScan, WifiKey,
  Calculator, Pomodoro, Stopwatch, Timer, SysInfo
};
View view = View::Dashboard;

// Simple immediate-execution calculator state.
String calcDisplay = "0";
double calcAccum = 0;
char calcOp = 0;      // pending operator: + - * /
bool calcFresh = true; // next digit starts a fresh entry

// Shared alert window: blinks the LED when a pomodoro/timer phase ends.
unsigned long alertUntilMs = 0;

// Pomodoro (adjustable work / break minutes).
bool pomoRunning = false;
bool pomoWork = true;
unsigned long pomoEndMs = 0;
int pomoWorkMin = 25;
int pomoBreakMin = 5;
int pomoRemainSec = 25 * 60;
int pomoDoneCount = 0;

// Stopwatch (count up).
bool swRunning = false;
unsigned long swStartMs = 0;
unsigned long swAccumMs = 0;

// Countdown timer.
bool tmRunning = false;
unsigned long tmEndMs = 0;
int tmSetSec = 5 * 60;
int tmRemainSec = 5 * 60;

// mm:ss (or h:mm:ss) clock string.
String fmtClock(long sec) {
  if (sec < 0) sec = 0;
  long h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
  char b[16];
  if (h > 0) snprintf(b, sizeof(b), "%ld:%02ld:%02ld", h, m, s);
  else snprintf(b, sizeof(b), "%02ld:%02ld", m, s);
  return String(b);
}

// On-device Wi-Fi entry state.
static constexpr int WIFI_SCAN_MAX = 8;
String wifiSsids[WIFI_SCAN_MAX];
int wifiSsidCount = 0;
String inputSsid;
String inputPassword;
bool kbShift = false;
bool kbSymbols = false;

// User-adjustable, persisted in NVS.
uint8_t dayBrightness = BRIGHTNESS_DAY;  // base brightness (settings -/+)
bool nightDimEnabled = true;             // auto-dim 23:00-07:00 on/off

// Low-quota alert threshold: remaining below this lights LED + tile border.
static constexpr int QUOTA_CRITICAL_REMAINING = 10;

// Push the back buffer to the panel. No-op when the sprite is unavailable
// (drawing already went straight to the panel).
void present() {
  if (useCanvas) canvas.pushSprite(0, 0);
}

// Fill the local time into `buf` as "HH:MM"; false if NTP has not synced yet.
bool clockText(char* buf, size_t n) {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  strftime(buf, n, "%H:%M", &t);
  return true;
}

#ifdef LCD_SMOKE_TEST
static constexpr uint32_t SMOKE_COLORS[] = {
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0x000000,
    0xFFFFFF,
};

static constexpr const char* SMOKE_NAMES[] = {
    "RED",
    "GREEN",
    "BLUE",
    "BLACK",
    "WHITE",
};

size_t smokeIndex = 0;
unsigned long lastSmokeMs = 0;

void drawSmokeFrame(size_t index) {
  uint32_t bg = SMOKE_COLORS[index];
  uint32_t fg = bg == 0x000000 ? 0xFFFFFF : 0x000000;

  gfx->fillScreen(bg);
  gfx->fillRect(0, 0, 80, 80, 0xFF0000);
  gfx->fillRect(80, 0, 80, 80, 0x00FF00);
  gfx->fillRect(160, 0, 80, 80, 0x0000FF);
  gfx->fillRect(240, 0, 80, 80, 0xFFFFFF);
  gfx->drawRect(0, 0, SCREEN_W, SCREEN_H, fg);
  gfx->drawRect(8, 8, SCREEN_W - 16, SCREEN_H - 16, fg);

  gfx->setTextColor(fg, bg);
  gfx->setTextSize(2);
  gfx->setCursor(28, 140);
  gfx->print("LCD SMOKE TEST");
  gfx->setTextSize(3);
  gfx->setCursor(72, 190);
  gfx->print(SMOKE_NAMES[index]);
  gfx->setTextSize(1);
  gfx->setCursor(34, 250);
  gfx->print("ST7796 / 8080 16-bit / 320x480");
  gfx->setCursor(34, 272);
  gfx->print("If this stays white, LCD config is wrong");
  present();
}
#endif

String htmlEscape(const String& value) {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  escaped.replace("\"", "&quot;");
  return escaped;
}

void loadSettings() {
  preferences.begin("dashboard", false);
  runtimeWifiSsid = preferences.isKey("ssid") ? preferences.getString("ssid") : String(WIFI_SSID);
  runtimeWifiPassword = preferences.isKey("pass") ? preferences.getString("pass") : String(WIFI_PASSWORD);
  runtimeApiUrl = preferences.isKey("api") ? preferences.getString("api") : String(DASHBOARD_API_URL);
  dayBrightness = preferences.isKey("bri") ? preferences.getUChar("bri") : BRIGHTNESS_DAY;
  nightDimEnabled = preferences.isKey("night") ? preferences.getBool("night") : true;
  applyTheme((Theme)(preferences.isKey("theme") ? preferences.getUChar("theme") : 0));
  uiLang = (Lang)(preferences.isKey("lang") ? preferences.getUChar("lang") : 0);
  pomoWorkMin = preferences.isKey("pwork") ? preferences.getUChar("pwork") : 25;
  pomoBreakMin = preferences.isKey("pbreak") ? preferences.getUChar("pbreak") : 5;
  preferences.end();
  pomoRemainSec = pomoWorkMin * 60;
  runtimeWifiSsid.trim();
  runtimeApiUrl.trim();
}

// Persist just the on-device display settings (brightness / night dim).
void saveDisplaySettings() {
  preferences.begin("dashboard", false);
  preferences.putUChar("bri", dayBrightness);
  preferences.putBool("night", nightDimEnabled);
  preferences.putUChar("theme", (uint8_t)uiTheme);
  preferences.putUChar("lang", (uint8_t)uiLang);
  preferences.putUChar("pwork", (uint8_t)pomoWorkMin);
  preferences.putUChar("pbreak", (uint8_t)pomoBreakMin);
  preferences.end();
}

void saveSettings(const String& ssid, const String& password, const String& apiUrl) {
  preferences.begin("dashboard", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.putString("api", apiUrl);
  preferences.end();
}

String activeApiUrl() {
  return runtimeApiUrl;
}

int clampPct(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

uint32_t colorForRemaining(int remainingPct) {
  if (remainingPct < 0) return COLOR_MUTED;
  if (remainingPct <= 15) return COLOR_DANGER;
  if (remainingPct <= 35) return COLOR_WARN;
  return COLOR_GOOD;
}

int remainingPct(const QuotaState& q) { return q.known ? 100 - clampPct(q.usedPct) : -1; }

bool quotaCritical(const QuotaState& q) {
  return q.known && remainingPct(q) < QUOTA_CRITICAL_REMAINING;
}

// True if either window of an agent is below the low-quota threshold.
bool agentCritical(const QuotaState& h5, const QuotaState& weekly) {
  return quotaCritical(h5) || quotaCritical(weekly);
}

bool anyQuotaCritical() {
  return agentCritical(state.codex5h, state.codexWeekly) ||
         agentCritical(state.claude5h, state.claudeWeekly);
}

// True when either usage bar has gone red (danger zone) — the agent is nearly
// out of quota, so its mascot nods off. Matches the bar colour exactly so the
// nap kicks in the moment the bar turns red.
bool agentDozing(const QuotaState& h5, const QuotaState& weekly) {
  return colorForRemaining(remainingPct(h5)) == COLOR_DANGER ||
         colorForRemaining(remainingPct(weekly)) == COLOR_DANGER;
}

// Compact token count: 3.3B / 12.3M / 456K / 789.
String formatTokens(int64_t tokens) {
  if (tokens < 0) return "--";
  if (tokens >= 1000000000LL) return String(tokens / 1000000000.0, 1) + "B";
  if (tokens >= 1000000LL) return String(tokens / 1000000.0, 1) + "M";
  if (tokens >= 1000LL) return String((long)(tokens / 1000)) + "K";
  return String((long)tokens);
}

// All-time API-equivalent cost as "$80,208" (thousands grouped). twd < 0 → "".
String formatTwd(int twd) {
  if (twd < 0) return "";
  String digits = String(twd);
  String out;
  int n = digits.length();
  for (int i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return "$" + out;
}

WeatherKind parseWeatherKind(const String& condition) {
  String c = condition;
  c.toLowerCase();
  if (c.indexOf("thunder") >= 0 || c.indexOf("storm") >= 0) return WeatherKind::Thunderstorm;
  if (c.indexOf("rain") >= 0 || c.indexOf("drizzle") >= 0 || c.indexOf("shower") >= 0) return WeatherKind::Rain;
  if (c.indexOf("fog") >= 0 || c.indexOf("mist") >= 0 || c.indexOf("haze") >= 0) return WeatherKind::Fog;
  if (c.indexOf("night_cloud") >= 0) return WeatherKind::NightCloudy;
  if (c.indexOf("night") >= 0) return WeatherKind::Night;
  if (c.indexOf("partly") >= 0) return WeatherKind::PartlyCloudy;
  if (c.indexOf("cloud") >= 0 || c.indexOf("overcast") >= 0) return WeatherKind::Cloudy;
  return WeatherKind::Sunny;
}

String labelForWeather(WeatherKind kind) {
  switch (kind) {
    case WeatherKind::Sunny:
      return "Sunny";
    case WeatherKind::PartlyCloudy:
      return "Partly cloudy";
    case WeatherKind::Cloudy:
      return "Cloudy";
    case WeatherKind::Rain:
      return "Rain";
    case WeatherKind::Thunderstorm:
      return "Thunderstorm";
    case WeatherKind::Fog:
      return "Fog";
    case WeatherKind::Night:
      return "Night";
    case WeatherKind::NightCloudy:
      return "Night cloudy";
  }
  return "Weather";
}

void drawText(int x, int y, const String& text, uint32_t color, int size = 1) {
  gfx->setTextColor(color, COLOR_BG);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(text);
}

void drawPanel(int x, int y, int w, int h) {
  gfx->fillRoundRect(x, y, w, h, 8, COLOR_PANEL);
  gfx->drawRoundRect(x, y, w, h, 8, COLOR_RULE);
}

bool pointInRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
  return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

// Concentric arc bands radiating from the dot (the usual Wi-Fi fan), instead
// of the old angular chevrons.
void drawWifiIcon(int cx, int y, bool connected) {
  uint32_t color = connected ? COLOR_GOOD : COLOR_MUTED;
  int cy = y + 16;
  gfx->fillCircle(cx, cy, 2, color);
  gfx->fillArc(cx, cy, 6, 8, 225, 315, color);
  gfx->fillArc(cx, cy, 11, 13, 225, 315, color);
  gfx->fillArc(cx, cy, 16, 18, 225, 315, color);
  if (!connected) {
    gfx->drawLine(cx - 12, y, cx + 11, y + 18, COLOR_DANGER);
    gfx->drawLine(cx - 11, y, cx + 12, y + 18, COLOR_DANGER);
  }
}

void drawSetupBackButton(bool pressed = false) {
  uint32_t bg = pressed ? COLOR_CODEX : COLOR_PANEL_2;
  uint32_t fg = pressed ? COLOR_BG : COLOR_TEXT;
  gfx->fillRoundRect(SETUP_BACK_X, SETUP_BACK_Y, SETUP_BACK_W, SETUP_BACK_H, 8, bg);
  gfx->drawRoundRect(SETUP_BACK_X, SETUP_BACK_Y, SETUP_BACK_W, SETUP_BACK_H, 8, COLOR_RULE);
  gfx->setTextColor(fg, bg);
  gfx->setTextSize(2);
  gfx->setCursor(SETUP_BACK_X + 31, SETUP_BACK_Y + 13);
  gfx->print("Back");
  present();
}

void drawSetupScreen() {
  gfx->fillScreen(COLOR_BG);
  drawPanel(12, 28, 296, 390);

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setTextSize(2);
  gfx->setCursor(34, 54);
  gfx->print("Wi-Fi Setup");

  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(34, 94);
  gfx->print("Connect to this AP");

  gfx->setTextColor(COLOR_CODEX, COLOR_PANEL);
  gfx->setTextSize(2);
  gfx->setCursor(34, 118);
  gfx->print("ESP32-Dashboard-");
  gfx->setCursor(34, 144);
  gfx->print("Setup");

  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(34, 184);
  gfx->print("Password");
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setCursor(34, 204);
  gfx->print(SETUP_AP_PASSWORD);

  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(34, 246);
  gfx->print("Open in browser");
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setTextSize(2);
  gfx->setCursor(34, 270);
  gfx->print("192.168.4.1");

  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(34, 330);
  gfx->print("Tap Back to cancel");
  drawSetupBackButton(false);
}

void drawSun(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, COLOR_WEATHER);
  gfx->fillCircle(cx - r / 3, cy - r / 3, max(3, r / 5), COLOR_STAR);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4.0f + (animationFrame % 8) * PI / 32.0f;
    int pulse = (animationFrame + i) % 2;
    int x1 = cx + cos(a) * (r + 7);
    int y1 = cy + sin(a) * (r + 7);
    int x2 = cx + cos(a) * (r + 13 + pulse * 4);
    int y2 = cy + sin(a) * (r + 13 + pulse * 4);
    gfx->drawLine(x1, y1, x2, y2, COLOR_WEATHER);
  }
}

void drawMoon(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, 0x94A3B8);
  gfx->fillCircle(cx - 2, cy - 2, r, COLOR_MOON);
  gfx->fillCircle(cx + r / 2, cy - r / 3, r, COLOR_PANEL);
}

void drawCloud(int x, int y, uint32_t color = COLOR_CLOUD) {
  gfx->fillCircle(x + 21, y + 32, 14, color);
  gfx->fillCircle(x + 42, y + 24, 20, color);
  gfx->fillCircle(x + 65, y + 34, 15, color);
  gfx->fillRoundRect(x + 13, y + 34, 66, 22, 9, color);
}

void drawRain(int x, int y) {
  drawCloud(x, y, COLOR_CLOUD);
  int phase = (animationFrame % 4) * 4;
  for (int i = 0; i < 4; ++i) {
    int rx = x + 20 + i * 16;
    int ry = y + 60 + ((phase + i * 5) % 20);
    gfx->fillTriangle(rx, ry, rx - 4, ry + 10, rx + 4, ry + 10, COLOR_RAIN);
    gfx->fillCircle(rx, ry + 9, 4, COLOR_RAIN);
  }
}

void drawThunderstorm(int x, int y) {
  drawRain(x, y);
  uint32_t bolt = (animationFrame % 2 == 0) ? COLOR_WARN : COLOR_WEATHER;
  gfx->fillTriangle(x + 50, y + 52, x + 38, y + 84, x + 58, y + 70, bolt);
  gfx->fillTriangle(x + 58, y + 70, x + 45, y + 102, x + 70, y + 63, bolt);
}

void drawFog(int x, int y) {
  drawCloud(x, y, COLOR_CLOUD);
  int drift = (animationFrame % 5) * 3;
  for (int i = 0; i < 4; ++i) {
    int yy = y + 64 + i * 8;
    gfx->drawFastHLine(x + 14 + drift, yy, 64, COLOR_MUTED);
    gfx->drawFastHLine(x + 34 - drift / 2, yy + 4, 42, 0x64748B);
  }
}

void drawWeatherIcon(int x, int y, WeatherKind kind) {
  switch (kind) {
    case WeatherKind::Sunny:
      drawSun(x + 54, y + 54, 28);
      break;
    case WeatherKind::PartlyCloudy:
      drawSun(x + 42, y + 40, 25);
      drawCloud(x + 38 + (animationFrame % 3), y + 54, COLOR_CLOUD);
      break;
    case WeatherKind::Cloudy:
      drawCloud(x + 16 + (animationFrame % 4), y + 46, COLOR_CLOUD);
      break;
    case WeatherKind::Rain:
      drawRain(x + 18, y + 34);
      break;
    case WeatherKind::Thunderstorm:
      drawThunderstorm(x + 18, y + 32);
      break;
    case WeatherKind::Fog:
      drawFog(x + 18, y + 34);
      break;
    case WeatherKind::Night:
      drawMoon(x + 52, y + 48, 28);
      gfx->fillCircle(x + 22, y + 26, (animationFrame % 2) ? 1 : 2, COLOR_STAR);
      gfx->fillCircle(x + 78, y + 25, 2, COLOR_STAR);
      if (animationFrame % 3 != 0) {
        gfx->drawFastHLine(x + 81, y + 72, 6, COLOR_STAR);
        gfx->drawFastVLine(x + 84, y + 69, 6, COLOR_STAR);
      }
      break;
    case WeatherKind::NightCloudy:
      drawMoon(x + 40, y + 36, 24);
      drawCloud(x + 32 + (animationFrame % 4), y + 52, COLOR_CLOUD);
      break;
  }
}

String fittedText(const String& text, size_t maxChars) {
  if (text.length() <= maxChars) return text;
  if (maxChars <= 1) return text.substring(0, maxChars);
  return text.substring(0, maxChars - 1) + "~";
}

String boundedText(const char* value, size_t maxChars) {
  String out = value ? String(value) : String("");
  out.trim();
  if (out.length() > maxChars) out = out.substring(0, maxChars);
  return out;
}

String boundedText(JsonVariantConst src, size_t maxChars, const String& fallback) {
  return src.is<const char*>() ? boundedText(src.as<const char*>(), maxChars) : fallback;
}

String twoDigit(const String& value) {
  return value.length() == 1 ? "0" + value : value;
}

// Convert a month abbreviation ("Jun") to a zero-padded number ("06").
// Falls back to the original string if it is already numeric or unknown.
String monthNum(const String& month) {
  static const char* names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 12; ++i) {
    if (month.startsWith(names[i])) return twoDigit(String(i + 1));
  }
  return month;
}

// "2026/06/30(Tue.)"
String formatDateLine() {
  return state.weather.year + "/" + monthNum(state.weather.month) + "/" +
         twoDigit(state.weather.day) + "(" + state.weather.weekday + ".)";
}

// One-line freshness/status shown at the bottom of the weather panel. Stays a
// static "online" while fresh; only counts up (in minutes) once data is stale.
String statusLine() {
  // Status words and time units stay English in both languages.
  if (!state.online) return "OFFLINE";
  unsigned long ageMin = (millis() - lastSuccessMs) / 60000;
  if (ageMin >= 5) return "updated " + String(ageMin) + "m ago";
  return "online";
}

// The collector sends English weekday/month/condition text; localise on-device
// so the JSON contract stays language-neutral.
String trWeekday(const String& weekday) {
  if (!zhUi()) return weekday;
  static const char* EN[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  static const char* ZH[7] = {"週一", "週二", "週三", "週四", "週五", "週六", "週日"};
  for (int i = 0; i < 7; ++i) {
    if (weekday.startsWith(EN[i])) return ZH[i];
  }
  return weekday;
}

String trMonth(const String& month) {
  if (!zhUi()) return month;
  static const char* EN[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 12; ++i) {
    if (month.startsWith(EN[i])) return String(i + 1) + "月";
  }
  return month;
}

// The collector reverse-geocodes the Chinese locality name (city_zh) so any
// place — not just a hardcoded list — localises; fall back to the English
// name when it is missing (offline/demo).
String trCity(const String& city) {
  if (zhUi() && state.weather.cityZh.length() > 0) return state.weather.cityZh;
  return city;
}

// US AQI banding: colour for the value, short label for the weather page.
uint32_t colorForAqi(int aqi) {
  if (aqi <= 50) return COLOR_GOOD;
  if (aqi <= 100) return COLOR_WARN;
  return COLOR_DANGER;
}

const char* aqiLabel(int aqi) {
  if (aqi <= 50) return tr("Good", "良好");
  if (aqi <= 100) return tr("Fair", "普通");
  if (aqi <= 150) return tr("Poor", "不良");
  return tr("Bad", "危害");
}

// UV index banding (WHO): colour for the value, short label for the tiles.
uint32_t colorForUv(int uv) {
  if (uv <= 2) return COLOR_GOOD;
  if (uv <= 5) return COLOR_WARN;
  return COLOR_DANGER;
}

const char* uvLabel(int uv) {
  if (uv <= 2) return tr("Low", "低");
  if (uv <= 5) return tr("Med", "中");
  if (uv <= 7) return tr("High", "高");
  if (uv <= 10) return tr("V.High", "很高");
  return tr("Extreme", "危險");
}

String trWeatherLabel() {
  if (!zhUi()) return state.weather.label;
  if (state.weather.rainAlert && !state.weather.isRaining) return "即將降雨";
  switch (state.weather.kind) {
    case WeatherKind::Sunny: return "晴天";
    case WeatherKind::PartlyCloudy: return "多雲時晴";
    case WeatherKind::Cloudy: return "陰天";
    case WeatherKind::Rain: return "降雨";
    case WeatherKind::Thunderstorm: return "雷雨";
    case WeatherKind::Fog: return "有霧";
    case WeatherKind::Night: return "晴朗夜晚";
    case WeatherKind::NightCloudy: return "夜間多雲";
  }
  return state.weather.label;
}

// Compact raindrop badge for the rain alert, sits to the right of "Rain %".
// Red = raining now, amber = high chance (gently blinks like the old banner).
void drawRainAlertIcon(int x, int y, bool urgent) {
  uint32_t c = urgent ? COLOR_DANGER : COLOR_WARN;
  if (!urgent && animationFrame % 2 == 1) c = COLOR_RAIN;
  gfx->fillTriangle(x + 7, y, x + 1, y + 9, x + 13, y + 9, c);
  gfx->fillCircle(x + 7, y + 10, 6, c);
}

void drawWeatherTile() {
  drawPanel(8, 8, 304, 224);

  drawWifiIcon(288, 18, WiFi.status() == WL_CONNECTED);

  char clock[8];
  if (clockText(clock, sizeof(clock))) {
    gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
    gfx->setTextSize(1);
    gfx->setCursor(238, 20);
    gfx->print(clock);
  }

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu56);
  gfx->setTextSize(1);
  gfx->setCursor(20, 18);
  gfx->print(fittedText(state.weather.day, 2));
  gfx->setFont(&fonts::Font2);

  setUiFont18();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(104, 30);
  gfx->print(fittedText(trMonth(state.weather.month), 8));
  gfx->setCursor(104, 52);
  gfx->print(fittedText(state.weather.year, 6));

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setCursor(104, 74);
  gfx->print(fittedText(trWeekday(state.weather.weekday), 7));
  gfx->setFont(&fonts::Font2);

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  setUiFont24();
  gfx->setCursor(188, 56);
  gfx->print(fittedText(trCity(state.weather.city), 9));
  gfx->setFont(&fonts::Font2);

  drawWeatherIcon(18, 92, state.weather.kind);

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu40);
  gfx->setCursor(186, 94);
  gfx->printf("%dC", state.weather.tempC);
  gfx->setFont(&fonts::Font2);

  setUiFont();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(184, 150);
  gfx->print(fittedText(trWeatherLabel(), 17));

  gfx->setTextColor(COLOR_RAIN, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(184, 178);
  gfx->printf(tr("Rain %d%%", "降雨 %d%%"), state.weather.rainPct);
  gfx->setFont(&fonts::Font2);

  if (state.weather.rainAlert) {
    drawRainAlertIcon(250, 174, state.weather.isRaining);
  }

  // Current air quality + UV, bottom-left corner (colour carries each level).
  gfx->setTextSize(1);
  int envX = 20;
  if (state.weather.aqi >= 0) {
    gfx->setTextColor(colorForAqi(state.weather.aqi), COLOR_PANEL);
    gfx->setCursor(envX, 216);
    gfx->printf("AQI %d", state.weather.aqi);
    envX = gfx->getCursorX() + 12;
  }
  if (state.weather.uv >= 0) {
    gfx->setTextColor(colorForUv(state.weather.uv), COLOR_PANEL);
    gfx->setCursor(envX, 216);
    gfx->printf("UV %d", state.weather.uv);
  }

  // Freshness line, right-aligned to the panel's bottom-right corner.
  setUiFont();
  gfx->setTextColor(state.online ? COLOR_MUTED : COLOR_DANGER, COLOR_PANEL);
  gfx->setTextSize(1);
  String st = fittedText(statusLine(), 22);
  gfx->setCursor(298 - gfx->textWidth(st), 216);
  gfx->print(st);
  gfx->setFont(&fonts::Font2);

  // Imminent-rain emphasis: pulse a coloured ring around the whole tile so you
  // know to grab an umbrella without opening the weather page. Blue while it is
  // actually raining, amber for a high-probability heads-up.
  if (state.weather.rainAlert) {
    uint32_t ring = state.weather.isRaining ? COLOR_RAIN : COLOR_WARN;
    if (animationFrame % 2 == 0) {
      gfx->drawRoundRect(8, 8, 304, 224, 14, ring);
      gfx->drawRoundRect(9, 9, 302, 222, 13, ring);
      gfx->drawRoundRect(10, 10, 300, 220, 12, ring);
    }
  }
}

void drawUsageBar(int x, int y, int w, int usedPct, uint32_t accent, bool known) {
  gfx->fillRoundRect(x, y, w, 12, 6, COLOR_PANEL_2);
  if (!known) {
    gfx->drawRoundRect(x, y, w, 12, 6, COLOR_MUTED);
    return;
  }
  int used = clampPct(usedPct);
  int remaining = 100 - used;
  uint32_t status = colorForRemaining(remaining);

  int fillW = (w * remaining) / 100;
  gfx->fillRoundRect(x, y, fillW, 12, 6, status);
  gfx->drawRoundRect(x, y, w, 12, 6, accent);
}

void drawQuotaBlock(int x, int y, int w, const char* label, const QuotaState& quota, uint32_t accent) {
  int used = quota.known ? clampPct(quota.usedPct) : 0;
  int remaining = remainingPct(quota);

  // Top row: window label (left) + reset countdown (right).
  setUiFont();
  gfx->setTextSize(1);
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(x, y);
  gfx->print(label);
  gfx->setFont(&fonts::Font2);
  gfx->setCursor(x + w - gfx->textWidth(quota.reset), y);
  gfx->print(quota.reset);

  // Big remaining %. "used %" is dropped here (redundant with the bar; the exact
  // value lives on the detail page) so the large number never collides. Sits a
  // little lower so the label's descenders (the y in "Weekly") aren't clipped.
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu40);
  gfx->setCursor(x, y + 16);
  if (quota.known) {
    gfx->printf("%d%%", remaining);
  } else {
    gfx->print("--%");
  }
  gfx->setFont(&fonts::Font2);

  drawUsageBar(x, y + 60, w, used, accent, quota.known);
}

// Blend two 0xRRGGBB colours channel-wise; t=0 → a, t=1 → b.
uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
  int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
  int r = ar + (int)((br - ar) * t);
  int g = ag + (int)((bg - ag) * t);
  int bl = ab + (int)((bb - ab) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

// Floating "z z z" for a dozing mascot: a trio drifting up-and-right off the
// head on a slow 8-frame loop. Each z is born small and bright near the head,
// then grows and fades as it climbs, wrapping back to the bottom — so even at
// the 1 fps render cadence the trail is always gently moving. CYCLE divides 256
// so the uint8_t animationFrame wrap stays seamless. Drawn transparently; the
// full-frame repaint clears old positions, so it leaves no residue.
void drawSleepZ(int x, int y, uint32_t color) {
  const uint8_t CYCLE = 8;
  const float base = (animationFrame % CYCLE) / (float)CYCLE;
  gfx->setFont(&fonts::Font0);
  for (int i = 0; i < 3; i++) {
    float p = base + i / 3.0f;      // stagger the trio along the path
    if (p >= 1.0f) p -= 1.0f;
    int px = x + (int)(p * 9.0f);   // drift right as it rises
    int py = y - (int)(p * 16.0f);  // climb up off the head
    gfx->setTextSize(1 + (int)(p * 2.0f));       // 1 → 3: small near head, big up top
    gfx->setTextColor(lerpColor(color, COLOR_PANEL, p * 0.85f));  // fade out near the top
    gfx->setCursor(px, py);
    gfx->print("z");
  }
  gfx->setTextSize(1);  // restore: callers set their own font but assume size 1
}

void drawClaudeMascot(int x, int y, bool doze) {
  bool blink = animationFrame % 8 == 0;
  gfx->fillRect(x + 8, y, 24, 6, COLOR_CLAUDE);
  gfx->fillRect(x + 4, y + 6, 32, 8, COLOR_CLAUDE);
  gfx->fillRect(x, y + 14, 40, 16, COLOR_CLAUDE);
  gfx->fillRect(x + 8, y + 30, 6, 6, COLOR_CLAUDE);
  gfx->fillRect(x + 26, y + 30, 6, 6, COLOR_CLAUDE);
  if (doze) {
    // Content sleepy curves ‿ ‿ — flat lash with the corners ticked up.
    gfx->drawFastHLine(x + 10, y + 16, 5, COLOR_BG);
    gfx->drawPixel(x + 9, y + 15, COLOR_BG);
    gfx->drawPixel(x + 15, y + 15, COLOR_BG);
    gfx->drawFastHLine(x + 25, y + 16, 5, COLOR_BG);
    gfx->drawPixel(x + 24, y + 15, COLOR_BG);
    gfx->drawPixel(x + 30, y + 15, COLOR_BG);
  } else if (blink) {
    gfx->drawFastHLine(x + 10, y + 16, 5, COLOR_BG);
    gfx->drawFastHLine(x + 25, y + 16, 5, COLOR_BG);
  } else {
    gfx->fillRect(x + 10, y + 14, 5, 5, COLOR_BG);
    gfx->fillRect(x + 25, y + 14, 5, 5, COLOR_BG);
  }
  gfx->fillRect(x - 4, y + 20, 4, 6, COLOR_CLAUDE);
  gfx->fillRect(x + 40, y + 20, 4, 6, COLOR_CLAUDE);
  if (doze) drawSleepZ(x + 38, y - 6, COLOR_TEXT);
}

void drawCodexMascot(int x, int y, bool doze) {
  bool blink = animationFrame % 8 == 4;
  gfx->fillRoundRect(x + 3, y + 2, 36, 30, 5, COLOR_CODEX);
  if (doze) {
    // Content sleepy curves ‿ ‿ — flat lash with the corners ticked up.
    gfx->drawFastHLine(x + 11, y + 13, 6, COLOR_BG);
    gfx->drawPixel(x + 10, y + 12, COLOR_BG);
    gfx->drawPixel(x + 17, y + 12, COLOR_BG);
    gfx->drawFastHLine(x + 25, y + 13, 6, COLOR_BG);
    gfx->drawPixel(x + 24, y + 12, COLOR_BG);
    gfx->drawPixel(x + 31, y + 12, COLOR_BG);
  } else if (blink) {
    gfx->drawFastHLine(x + 11, y + 13, 6, COLOR_BG);
    gfx->drawFastHLine(x + 25, y + 13, 6, COLOR_BG);
  } else {
    gfx->fillRect(x + 11, y + 10, 6, 6, COLOR_BG);
    gfx->fillRect(x + 25, y + 10, 6, 6, COLOR_BG);
  }
  gfx->drawFastHLine(x + 14, y + 22, 14, COLOR_BG);
  gfx->drawFastHLine(x + 15, y + 23, 12, COLOR_BG);
  gfx->drawFastHLine(x, y + 12, 6, COLOR_CODEX);
  gfx->drawFastHLine(x + 36, y + 12, 6, COLOR_CODEX);
  gfx->drawFastVLine(x + 14, y - 2, 6, COLOR_CODEX);
  gfx->drawFastVLine(x + 28, y - 2, 6, COLOR_CODEX);
  if (doze) drawSleepZ(x + 34, y - 4, COLOR_TEXT);
}

void drawAgentTile(int x, int y, const String& title, uint32_t accent, const QuotaState& h5, const QuotaState& weekly, bool codex) {
  drawPanel(x, y, 146, 224);
  bool doze = agentDozing(h5, weekly);
  // A dozing mascot nods slowly (2px, every other frame) instead of the lively
  // 4-frame bob, so a maxed-out agent reads as sleepy rather than perky.
  int bob = doze ? (animationFrame % 2 ? 1 : 0)
                 : ((animationFrame % 4 == 1) ? -1 : ((animationFrame % 4 == 3) ? 1 : 0));
  if (codex) {
    drawCodexMascot(x + 8, y + 15 + bob, doze);
  } else {
    drawClaudeMascot(x + 8, y + 14 + bob, doze);
  }

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(x + 56, y + 22);
  gfx->print(title);
  gfx->setFont(&fonts::Font2);

  // Window labels stay English in Chinese mode too (time units untranslated).
  drawQuotaBlock(x + 14, y + 64, 118, "5hr", h5, accent);
  drawQuotaBlock(x + 14, y + 148, 118, "Weekly", weekly, accent);
}

void drawDashboard() {
  gfx->fillScreen(COLOR_BG);

  drawWeatherTile();
  drawAgentTile(8, 248, "Codex", COLOR_CODEX, state.codex5h, state.codexWeekly, true);
  drawAgentTile(166, 248, "Claude", COLOR_CLAUDE, state.claude5h, state.claudeWeekly, false);
  present();
}

void returnToDashboardFromSetup();

void handleSetupRoot() {
  String html = F(
      "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>ESP32 Dashboard Setup</title>"
      "<style>"
      "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#101215;color:#f3f6fa}"
      "main{max-width:520px;margin:0 auto;padding:24px}"
      "h1{font-size:24px;margin:0 0 18px}"
      "label{display:block;margin:14px 0 6px;color:#94a3b8}"
      "input{box-sizing:border-box;width:100%;font-size:16px;padding:12px;border:1px solid #323a45;border-radius:8px;background:#1c2128;color:#f3f6fa}"
      "button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:8px;background:#38bdf8;color:#071014;font-size:17px;font-weight:700}"
      "button.secondary{background:#242b33;color:#f3f6fa;border:1px solid #323a45}"
      "p{color:#94a3b8;line-height:1.45}"
      "</style></head><body><main><h1>ESP32 Dashboard Setup</h1>"
      "<p>Save Wi-Fi and JSON endpoint. The device reboots after saving.</p>"
      "<form method=\"post\" action=\"/save\">");
  html += F("<label>Wi-Fi SSID</label><input name=\"ssid\" required value=\"");
  html += htmlEscape(runtimeWifiSsid);
  html += F("\"><label>Wi-Fi password</label><input name=\"pass\" type=\"password\" placeholder=\"Leave blank to keep current\">");
  html += F("<label>Dashboard JSON URL</label><input name=\"api\" placeholder=\"http://your-server-host/dashboard.json\" value=\"");
  html += htmlEscape(runtimeApiUrl);
  html += F("\"><button type=\"submit\">Save and reboot</button></form>"
            "<form method=\"post\" action=\"/cancel\"><button class=\"secondary\" type=\"submit\">Back to dashboard</button></form>"
            "</main></body></html>");
  setupServer.send(200, "text/html", html);
}

void handleSetupSave() {
  String ssid = setupServer.arg("ssid");
  String password = setupServer.arg("pass");
  String apiUrl = setupServer.arg("api");
  ssid.trim();
  apiUrl.trim();
  if (password.length() == 0) password = runtimeWifiPassword;

  saveSettings(ssid, password, apiUrl);
  runtimeWifiSsid = ssid;
  runtimeWifiPassword = password;
  runtimeApiUrl = apiUrl;

  setupServer.send(200, "text/html",
                   "<!doctype html><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                   "<body style=\"font-family:system-ui;background:#101215;color:#f3f6fa;padding:24px\">"
                   "<h1>Saved</h1><p>ESP32 is rebooting.</p></body>");
  Serial.println("Settings saved; rebooting");
  delay(900);
  ESP.restart();
}

void handleSetupCancel() {
  setupServer.send(200, "text/html",
                   "<!doctype html><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                   "<body style=\"font-family:system-ui;background:#101215;color:#f3f6fa;padding:24px\">"
                   "<h1>Returning</h1><p>Going back to the dashboard.</p></body>");
  delay(250);
  returnToDashboardFromSetup();
}

void handleSetupNotFound() {
  setupServer.sendHeader("Location", "/", true);
  setupServer.send(302, "text/plain", "");
}

void startWifiSetupPortal() {
  if (setupMode) return;

  setupMode = true;
  state.online = false;
  state.source = "setup";
  Serial.println("Starting Wi-Fi setup portal");
  drawSetupScreen();

  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(SETUP_DNS_PORT, "*", apIp);

  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.on("/cancel", HTTP_POST, handleSetupCancel);
  setupServer.onNotFound(handleSetupNotFound);
  setupServer.begin();

  Serial.printf("Setup AP: %s (%s), http://%s, start=%s\n", SETUP_AP_SSID, SETUP_AP_PASSWORD,
                apIp.toString().c_str(), apOk ? "ok" : "failed");
}

void handleSetupTouch() {
  if (!lcd.touch()) return;

  uint16_t x = 0;
  uint16_t y = 0;
  bool touching = lcd.getTouch(&x, &y);

  if (!touching) {
    touchWasDown = false;
    return;
  }

  if (touchWasDown) return;
  touchWasDown = true;

  if (pointInRect(x, y, SETUP_BACK_X, SETUP_BACK_Y, SETUP_BACK_W, SETUP_BACK_H)) {
    Serial.printf("Setup back tapped at x=%u y=%u\n", x, y);
    drawSetupBackButton(true);
    delay(120);
    returnToDashboardFromSetup();
  }
}

// Defined further down (after the data/render helpers they depend on).
void handleTap(uint16_t x, uint16_t y);
void handleLongPress();

void handleTouch() {
  if (setupMode || !lcd.touch()) return;

  uint16_t x = 0;
  uint16_t y = 0;
  bool touching = lcd.getTouch(&x, &y);
  if (touching) {
    lastTouchX = x;
    lastTouchY = y;
  }

  if (!touching) {
    // Release: a short press (no long-press already fired) is a tap.
    if (touchWasDown && !longPressFired) handleTap(lastTouchX, lastTouchY);
    touchWasDown = false;
    longPressFired = false;
    return;
  }

  if (!touchWasDown) {
    touchWasDown = true;
    longPressFired = false;
    touchStartMs = millis();
    return;
  }

  if (!longPressFired && millis() - touchStartMs >= LONG_PRESS_MS) {
    longPressFired = true;
    handleLongPress();
  }
}

void applyQuota(JsonVariantConst src, QuotaState& dst) {
  dst.tokens = -1;
  dst.costTwd = -1;
  dst.known = false;
  dst.usedPct = 0;
  if (src.isNull()) {
    dst.reset = "--";
    return;
  }
  if (src["used_pct"].is<int>()) {
    dst.usedPct = clampPct(src["used_pct"].as<int>());
    dst.known = true;
  }
  const char* status = src["status"].is<const char*>() ? src["status"].as<const char*>() : nullptr;
  if (!dst.known && status && (strcmp(status, "exhausted") == 0 || strcmp(status, "limited") == 0)) {
    dst.usedPct = 100;
    dst.known = true;
  }
  if (src["reset"].is<const char*>()) {
    dst.reset = boundedText(src["reset"].as<const char*>(), MAX_RESET_TEXT);
  } else if (!dst.known) {
    dst.reset = "--";
  }
  if (src["tokens"].is<int64_t>()) {
    int64_t tokens = src["tokens"].as<int64_t>();
    dst.tokens = tokens > 0 ? tokens : 0;
  }
  if (src["cost_twd"].is<int>()) {
    int cost = src["cost_twd"].as<int>();
    dst.costTwd = cost >= 0 ? cost : -1;
  }
}

// Parse an agent's daily token series + accumulated totals for the chart.
void applyChart(JsonVariantConst agent, AgentChart& chart) {
  chart.dailyCount = 0;
  chart.totalTok = -1;
  chart.totalTwd = -1;
  for (JsonVariantConst v : agent["daily"].as<JsonArrayConst>()) {
    if (chart.dailyCount >= DAILY_MAX) break;
    long value = v.as<long>();
    chart.daily[chart.dailyCount++] = value > 0 ? value : 0;
  }
  if (agent["total_tok"].is<int64_t>()) chart.totalTok = agent["total_tok"].as<int64_t>();
  if (agent["total_twd"].is<int>()) chart.totalTwd = agent["total_twd"].as<int>();
  chart.modelCount = 0;
  for (JsonVariantConst v : agent["models"].as<JsonArrayConst>()) {
    if (chart.modelCount >= MODEL_MAX) break;
    if (!v["name"].is<const char*>()) continue;
    ModelShare& share = chart.models[chart.modelCount++];
    share.name = boundedText(v["name"].as<const char*>(), 20);
    share.pct = clampPct(v["pct"].as<int>());
    share.tok = v["tok"].is<int64_t>() ? v["tok"].as<int64_t>() : 0;
  }
}

bool readHttpBody(HTTPClient& http, String& payload) {
  int length = http.getSize();
  if (length > (int)DASHBOARD_MAX_PAYLOAD_BYTES) return false;

  WiFiClient* stream = http.getStreamPtr();
  payload = "";
  size_t reserveSize = (length > 0) ? (size_t)length : 1024;
  if (reserveSize > DASHBOARD_MAX_PAYLOAD_BYTES) reserveSize = DASHBOARD_MAX_PAYLOAD_BYTES;
  payload.reserve(reserveSize);

  char buffer[257];
  size_t total = 0;
  unsigned long lastReadMs = millis();
  while ((http.connected() || stream->available()) && (length < 0 || total < (size_t)length)) {
    size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastReadMs > 5000) return false;
      delay(1);
      continue;
    }

    if (total >= DASHBOARD_MAX_PAYLOAD_BYTES) return false;
    size_t toRead = available;
    if (toRead > sizeof(buffer) - 1) toRead = sizeof(buffer) - 1;
    if (length >= 0) {
      size_t remaining = (size_t)length - total;
      if (toRead > remaining) toRead = remaining;
    }
    size_t room = DASHBOARD_MAX_PAYLOAD_BYTES - total;
    if (toRead > room) toRead = room;

    size_t got = stream->readBytes(buffer, toRead);
    if (got == 0) {
      if (millis() - lastReadMs > 5000) return false;
      continue;
    }
    buffer[got] = '\0';
    payload += buffer;
    total += got;
    lastReadMs = millis();
  }

  return length < 0 || total == (size_t)length;
}

bool fetchDashboardState() {
  String url = activeApiUrl();
  if (url.length() == 0 || WiFi.status() != WL_CONNECTED) {
    state.online = false;
    state.source = "demo";
    Serial.println("Dashboard API disabled or Wi-Fi offline; using demo data");
    return false;
  }

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(url)) {
    state.online = false;
    state.source = "http begin failed";
    Serial.println("Dashboard API begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    state.online = false;
    state.source = String("http ") + code;
    Serial.printf("Dashboard API returned HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBody(http, payload)) {
    state.online = false;
    state.source = "payload too large";
    Serial.println("Dashboard API payload rejected");
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    state.online = false;
    state.source = "json error";
    Serial.printf("Dashboard API JSON error: %s\n", err.c_str());
    return false;
  }

  JsonVariantConst weather = doc["weather"];
  state.weather.city = boundedText(weather["city"], MAX_LABEL_TEXT, state.weather.city);
  state.weather.weekday = boundedText(weather["weekday"], MAX_SHORT_TEXT, state.weather.weekday);
  state.weather.date = boundedText(weather["date"], MAX_LABEL_TEXT, state.weather.date);
  if (weather["day"].is<int>()) state.weather.day = String(weather["day"].as<int>());
  if (weather["day"].is<const char*>()) state.weather.day = boundedText(weather["day"].as<const char*>(), MAX_SHORT_TEXT);
  state.weather.month = boundedText(weather["month"], MAX_SHORT_TEXT, state.weather.month);
  if (weather["year"].is<int>()) state.weather.year = String(weather["year"].as<int>());
  if (weather["year"].is<const char*>()) state.weather.year = boundedText(weather["year"].as<const char*>(), MAX_SHORT_TEXT);
  if (state.weather.day.length() == 0 && state.weather.date.length() > 0) {
    int split = state.weather.date.lastIndexOf(' ');
    if (split > 0) {
      state.weather.month = state.weather.date.substring(0, split);
      state.weather.day = state.weather.date.substring(split + 1);
    }
  }
  if (weather["temp_c"].is<int>()) state.weather.tempC = weather["temp_c"].as<int>();
  if (weather["rain_pct"].is<int>()) state.weather.rainPct = clampPct(weather["rain_pct"].as<int>());
  if (weather["rain_mm"].is<float>() || weather["rain_mm"].is<int>()) {
    state.weather.rainMm = weather["rain_mm"].as<float>();
    if (state.weather.rainMm < 0.0f) state.weather.rainMm = 0.0f;
  }
  if (weather["is_raining"].is<bool>()) state.weather.isRaining = weather["is_raining"].as<bool>();
  if (weather["rain_alert"].is<bool>()) state.weather.rainAlert = weather["rain_alert"].as<bool>();
  if (state.weather.rainMm > 0.0f) state.weather.isRaining = true;
  state.weather.aqi = weather["aqi"].is<int>() ? weather["aqi"].as<int>() : -1;
  state.weather.uv = weather["uv"].is<int>() ? weather["uv"].as<int>() : -1;
  state.weather.sunrise = boundedText(weather["sunrise"], MAX_SHORT_TEXT, "");
  state.weather.sunset = boundedText(weather["sunset"], MAX_SHORT_TEXT, "");
  state.weather.cityZh = boundedText(weather["city_zh"], MAX_LABEL_TEXT, "");
  if (weather["condition"].is<const char*>()) {
    state.weather.kind = parseWeatherKind(boundedText(weather["condition"].as<const char*>(), MAX_LABEL_TEXT));
    state.weather.label = labelForWeather(state.weather.kind);
  }
  state.weather.label = boundedText(weather["label"], MAX_LABEL_TEXT, state.weather.label);

  state.weather.hourlyCount = 0;
  JsonArrayConst hourly = weather["hourly"].as<JsonArrayConst>();
  for (JsonVariantConst h : hourly) {
    if (state.weather.hourlyCount >= MAX_HOURLY) break;
    HourlyPoint& p = state.weather.hourly[state.weather.hourlyCount];
    p.hour = h["t"].is<const char*>() ? boundedText(h["t"].as<const char*>(), MAX_SHORT_TEXT) : String(h["t"].as<int>());
    p.tempC = h["temp"].as<int>();
    p.rainPct = clampPct(h["rain"].as<int>());
    state.weather.hourlyCount++;
  }

  applyQuota(doc["claude"]["h5"], state.claude5h);
  applyQuota(doc["claude"]["weekly"], state.claudeWeekly);
  applyQuota(doc["codex"]["h5"], state.codex5h);
  applyQuota(doc["codex"]["weekly"], state.codexWeekly);
  applyChart(doc["claude"], state.claudeChart);
  applyChart(doc["codex"], state.codexChart);

  state.online = true;
  state.source = "api";
  lastSuccessMs = millis();
  if (doc["meta"]["source"].is<const char*>()) state.source = boundedText(doc["meta"]["source"].as<const char*>(), MAX_SOURCE_TEXT);
  Serial.println("Dashboard API updated");
  return true;
}

// Start an NTP sync so the on-screen clock and night dimming have real time.
// Non-blocking: getLocalTime() elsewhere simply reports "not ready" until sync.
void startNtp() {
  configTzTime(CLOCK_TZ, NTP_SERVER_1, NTP_SERVER_2);
  struct tm t;
  timeSynced = getLocalTime(&t, 3000);
  Serial.printf("NTP sync: %s\n", timeSynced ? "ok" : "pending");
}

void connectWifi() {
  if (runtimeWifiSsid.length() == 0) {
    Serial.println("Wi-Fi SSID is blank; staying in demo mode");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(runtimeWifiSsid.c_str(), runtimeWifiPassword.c_str());
  Serial.printf("Connecting to Wi-Fi SSID %s\n", runtimeWifiSsid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    startNtp();
  } else {
    Serial.println("Wi-Fi connect timeout; using demo data");
  }
}

// Called from the main loop while disconnected; nudges a reconnect and, once
// back online, re-syncs NTP. Rate-limited by RECONNECT_INTERVAL_MS.
void maybeReconnectWifi() {
  if (runtimeWifiSsid.length() == 0) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!timeSynced) startNtp();
    return;
  }
  if (millis() - lastReconnectMs < RECONNECT_INTERVAL_MS) return;
  lastReconnectMs = millis();
  Serial.println("Wi-Fi disconnected; attempting reconnect");
  WiFi.reconnect();
}

bool refreshDashboardConnectionNow() {
  bool ok = false;
  for (int attempt = 1; attempt <= DASHBOARD_FETCH_ATTEMPTS; ++attempt) {
    if (runtimeWifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
      connectWifi();
    } else if (WiFi.status() == WL_CONNECTED && !timeSynced) {
      startNtp();
    }
    ok = fetchDashboardState();
    if (ok) break;
    if (attempt < DASHBOARD_FETCH_ATTEMPTS) {
      Serial.printf("Dashboard refresh retry %d/%d\n", attempt + 1, DASHBOARD_FETCH_ATTEMPTS);
      if (runtimeWifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
      }
      delay(500);
    }
  }
  lastFetchMs = millis();
  lastRenderMs = millis();
  return ok;
}

// Apply the effective backlight level: the user's day brightness, dropped to
// BRIGHTNESS_NIGHT at night when night dim is enabled. Only writes on change.
void applyAutoBrightness() {
  uint8_t target = dayBrightness;
  struct tm t;
  if (nightDimEnabled && getLocalTime(&t, 0) &&
      (t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR)) {
    target = BRIGHTNESS_NIGHT;
  }
  if (target != currentBrightness) {
    currentBrightness = target;
    lcd.setBrightness(target);
  }
}

void returnToDashboardFromSetup() {
  if (!setupMode) return;

  Serial.println("Leaving Wi-Fi setup portal");
  setupMode = false;
  touchWasDown = false;
  longPressFired = false;

  setupServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);

  gfx->fillScreen(COLOR_BG);
  drawPanel(32, 166, 256, 126);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setTextSize(2);
  gfx->setCursor(62, 200);
  gfx->print("Returning...");
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(62, 238);
  gfx->print("Reconnecting Wi-Fi");
  present();

  refreshDashboardConnectionNow();
  view = View::Dashboard;
  drawDashboard();
}

// ---------------------------------------------------------------------------
// Touch pages: detail / weather / settings, plus navigation.
// ---------------------------------------------------------------------------

// Back button (top-left of every sub-page) and settings hit rectangles.
static constexpr int BACK_X = 16, BACK_Y = 16, BACK_W = 96, BACK_H = 40;

// Settings layout (sectioned app-grid).
static constexpr int SET_ROW_X = 16, SET_ROW_W = 288, SET_ROW_H = 44;
static constexpr int BRI_ROW_Y = 78;
static constexpr int BRI_MINUS_X = 172, BRI_PLUS_X = 264, BRI_BTN_WH = 40;
static constexpr int NIGHT_ROW_Y = 126;
static constexpr int NIGHT_TGL_X = 196, NIGHT_TGL_W = 108, NIGHT_TGL_H = 40;
static constexpr int THEME_ROW_Y = 174;
static constexpr int THEME_SW_Y = 178, THEME_SW_W = 44, THEME_SW_H = 34, THEME_SW_X0 = 150, THEME_SW_GAP = 52;
static constexpr int APP_W = 92, APP_H = 58, APP_GAP = 6, APP_X0 = 14;
static constexpr int APP_ROW1_Y = 244, APP_ROW2_Y = 308;
static constexpr int SYS_Y = 396, SYS_W = 92, SYS_H = 42;

// App tile rect for index 0..4 (row-major, 3 per row).
void appTileRect(int idx, int& x, int& y) {
  x = APP_X0 + (idx % 3) * (APP_W + APP_GAP);
  y = (idx / 3 == 0) ? APP_ROW1_Y : APP_ROW2_Y;
}

// System button rect for column 0..2.
void sysBtnRect(int col, int& x) { x = APP_X0 + col * (APP_W + APP_GAP); }

void drawBackButton() {
  gfx->fillRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 8, COLOR_PANEL_2);
  gfx->drawRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 8, COLOR_RULE);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  setUiFont18();
  gfx->setCursor(BACK_X + 14, BACK_Y + 10);
  gfx->print(tr("< Back", "< 返回"));
  gfx->setFont(&fonts::Font2);
}

// TokenBar-style daily bar chart. Bars scaled to the largest day; baseline at
// the bottom of the [x,y,w,h] box.
void drawBarChart(int x, int y, int w, int h, const long* daily, int n, uint32_t color) {
  if (n <= 0) return;
  long maxv = 1;
  for (int i = 0; i < n; ++i) {
    if (daily[i] > maxv) maxv = daily[i];
  }
  int slot = w / n;
  int barW = slot > 5 ? slot - 4 : (slot > 1 ? slot - 1 : 1);
  int baseline = y + h;
  for (int i = 0; i < n; ++i) {
    int bh = (int)((daily[i] * (long)h) / maxv);
    if (daily[i] > 0 && bh < 2) bh = 2;
    int bx = x + i * slot + (slot - barW) / 2;
    gfx->fillRoundRect(bx, baseline - bh, barW, bh, 2, daily[i] > 0 ? color : COLOR_PANEL_2);
  }
  gfx->drawFastHLine(x, baseline, w, COLOR_RULE);
}

// Brightness-scaled shade of a 24-bit colour (model-mix bar segments).
uint32_t scaleColor(uint32_t color, uint8_t num, uint8_t den) {
  uint32_t r = ((color >> 16) & 0xFF) * num / den;
  uint32_t g = ((color >> 8) & 0xFF) * num / den;
  uint32_t b = (color & 0xFF) * num / den;
  return (r << 16) | (g << 8) | b;
}

// Compact one-line window summary: "5h  82% left      4h1m".
void drawRateSummary(int x, int y, const char* label, const QuotaState& quota) {
  int rem = remainingPct(quota);
  setUiFont18();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(x, y);
  gfx->print(label);
  gfx->setTextColor(quota.known ? colorForRemaining(rem) : COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(x + 44, y);
  if (quota.known) {
    gfx->printf(tr("%d%% left", "剩 %d%%"), rem);
  } else {
    gfx->print(tr("--% left", "剩 --%"));
  }
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(x + 208, y);
  gfx->print(quota.reset);
  gfx->setFont(&fonts::Font2);
}

void drawDetailPage(const char* title, uint32_t accent, const QuotaState& h5,
                    const QuotaState& weekly, const AgentChart& chart) {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();

  gfx->setTextColor(accent, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(300 - gfx->textWidth(title), 20);
  gfx->print(title);
  gfx->setFont(&fonts::Font2);

  // Model mix over the chart window is the hero. Quota %/reset already live on
  // the main tiles and the old NT$ figure was API-equivalent rather than real
  // spend, so this page leads with the one thing only it can show: which
  // models the tokens went to. Stacked share bar + per-model legend.
  setUiFont();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(28, 70);
  gfx->printf(tr("MODELS - %dD", "模型分佈 - %dD"), DAILY_MAX);

  const int barX = 28, barY = 86, barWidth = 256, barHeight = 18;
  if (chart.modelCount == 0) {
    gfx->fillRoundRect(barX, barY, barWidth, barHeight, 4, COLOR_PANEL_2);
    gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
    gfx->setCursor(barX, barY + barHeight + 8);
    gfx->print(tr("no local usage data", "無本機用量資料"));
  } else {
    gfx->setFont(&fonts::Font2);
    int cum = 0;
    for (int i = 0; i < chart.modelCount; ++i) {
      uint32_t shade = scaleColor(accent, (uint8_t)(8 - 3 * i), 8);
      int x0 = barX + cum * barWidth / 100;
      cum += chart.models[i].pct;
      if (cum > 100) cum = 100;
      // Last segment absorbs integer-division slack so the bar fills exactly.
      int x1 = (i == chart.modelCount - 1) ? barX + barWidth
                                           : barX + cum * barWidth / 100;
      gfx->fillRect(x0, barY, x1 - x0, barHeight, shade);

      int rowY = barY + barHeight + 8 + i * 18;
      gfx->fillRect(barX, rowY + 3, 10, 10, shade);
      gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
      gfx->setCursor(barX + 16, rowY);
      gfx->print(fittedText(chart.models[i].name, 22));
      String stat = formatTokens(chart.models[i].tok) + "  " + String(chart.models[i].pct) + "%";
      gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
      gfx->setCursor(barX + barWidth - gfx->textWidth(stat), rowY);
      gfx->print(stat);
    }
  }

  // Daily bar chart (last DAILY_MAX days).
  drawBarChart(28, 166, 256, 126, chart.daily, chart.dailyCount, accent);
  setUiFont();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(28, 300);
  gfx->printf("%dd ago", chart.dailyCount > 0 ? chart.dailyCount : DAILY_MAX);
  gfx->setCursor(256, 300);
  gfx->print("today");
  gfx->setFont(&fonts::Font2);

  gfx->drawFastHLine(28, 326, 256, COLOR_RULE);

  // Compact rate-limit summary for both windows.
  drawRateSummary(28, 344, "5h", h5);
  drawRateSummary(28, 378, "Wk", weekly);

  // Per-window tokens plus the all-time lifetime total, small.
  setUiFont();
  gfx->setTextSize(1);
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(28, 410);
  gfx->printf("5h %s tok", formatTokens(h5.tokens).c_str());
  gfx->setCursor(28, 428);
  gfx->printf("Wk %s tok", formatTokens(weekly.tokens).c_str());
  gfx->setCursor(28, 446);
  gfx->printf(tr("All-time %s tok", "累計 %s tok"), formatTokens(chart.totalTok).c_str());

  // API-equivalent cost is this page's headline number, so make it pop: the
  // agent's accent colour + a big face, right-aligned on the all-time row. The
  // leading "~" reads as "about / equivalent" so it isn't mistaken for a bill.
  String cost = formatTwd(chart.totalTwd);
  if (cost.length()) {
    String big = "~" + cost;
    gfx->setFont(&fonts::DejaVu24);
    gfx->setTextColor(accent, COLOR_PANEL);
    gfx->setCursor(284 - gfx->textWidth(big), 438);
    gfx->print(big);
  }
  gfx->setFont(&fonts::Font2);
  present();
}

void drawWeatherPage() {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();

  gfx->setTextColor(COLOR_WEATHER, COLOR_PANEL);
  setUiFont24();
  gfx->setCursor(300 - gfx->textWidth(tr("Weather", "天氣")), 20);
  gfx->print(tr("Weather", "天氣"));
  gfx->setFont(&fonts::Font2);

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  setUiBigFont();
  gfx->setCursor(28, 78);
  gfx->print(fittedText(trCity(state.weather.city), 12));
  resetUiBigFont();

  // Current temperature: size5 number + smaller "C" (size6 crowded neighbours
  // and the 'C' glyph smeared together).
  gfx->setTextSize(5);
  gfx->setCursor(28, 110);
  String tempStr = String(state.weather.tempC);
  gfx->print(tempStr);
  int tempW = gfx->textWidth(tempStr);
  gfx->setTextSize(3);
  gfx->setCursor(28 + tempW + 8, 122);
  gfx->print("C");

  // Capped at 11 chars so it cannot run into the AQI/sun column at x=188.
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  setUiBigFont();
  gfx->setCursor(28, 178);
  gfx->print(fittedText(trWeatherLabel(), 11));

  gfx->setTextColor(COLOR_RAIN, COLOR_PANEL);
  gfx->setCursor(28, 210);
  gfx->printf(tr("Rain %d%%", "降雨 %d%%"), state.weather.rainPct);
  resetUiBigFont();

  // Icon sits high in the top-right so its cloud clears the info column below.
  drawWeatherIcon(200, 60, state.weather.kind);

  // Air quality / UV / sun times, stacked under the icon. Lines with no data
  // are skipped so the column just gets shorter.
  setUiFont();
  gfx->setTextSize(1);
  int infoY = 186;
  if (state.weather.aqi >= 0) {
    gfx->setTextColor(colorForAqi(state.weather.aqi), COLOR_PANEL);
    gfx->setCursor(188, infoY);
    gfx->printf("AQI %d %s", state.weather.aqi, aqiLabel(state.weather.aqi));
    infoY += 20;
  }
  if (state.weather.uv >= 0) {
    gfx->setTextColor(colorForUv(state.weather.uv), COLOR_PANEL);
    gfx->setCursor(188, infoY);
    gfx->printf("UV %d %s", state.weather.uv, uvLabel(state.weather.uv));
    infoY += 20;
  }
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  if (state.weather.sunrise.length() > 0) {
    gfx->setCursor(188, infoY);
    gfx->printf(tr("Sunrise %s", "日出 %s"), state.weather.sunrise.c_str());
    infoY += 20;
  }
  if (state.weather.sunset.length() > 0) {
    gfx->setCursor(188, infoY);
    gfx->printf(tr("Sunset %s", "日落 %s"), state.weather.sunset.c_str());
  }
  gfx->setFont(&fonts::Font2);

  gfx->drawFastHLine(28, 268, 256, COLOR_RULE);
  setUiFont();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(28, 280);
  gfx->print(tr("NEXT HOURS - RAIN %", "未來數小時 - 降雨機率"));
  gfx->setFont(&fonts::Font2);

  if (state.weather.hourlyCount > 0) {
    int cols = state.weather.hourlyCount;
    int chartX = 20;
    int chartW = 272;       // wider columns to un-cram the labels
    int baseline = 430;     // bar baseline
    int maxBar = 66;        // 100% rain height
    for (int i = 0; i < cols; ++i) {
      const HourlyPoint& p = state.weather.hourly[i];
      int cx = chartX + (chartW * (2 * i + 1)) / (2 * cols);

      // hour label (top)
      gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
      gfx->setTextSize(2);
      String hs = p.hour;
      gfx->setCursor(cx - gfx->textWidth(hs) / 2, 298);
      gfx->print(hs);

      // temperature (well below the hour so they don't crowd)
      gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
      gfx->setTextSize(1);
      String ts = String(p.tempC) + "C";
      gfx->setCursor(cx - gfx->textWidth(ts) / 2, 330);
      gfx->print(ts);

      // rain-probability bar
      int bh = (p.rainPct * maxBar) / 100;
      if (bh < 2) bh = 2;
      gfx->fillRoundRect(cx - 7, baseline - bh, 14, bh, 3, p.rainPct > 0 ? COLOR_RAIN : COLOR_PANEL_2);

      // rain % (below bar)
      gfx->setTextColor(COLOR_RAIN, COLOR_PANEL);
      String rs = String(p.rainPct) + "%";
      gfx->setCursor(cx - gfx->textWidth(rs) / 2, baseline + 8);
      gfx->print(rs);
    }
    gfx->drawFastHLine(chartX, baseline, chartW, COLOR_RULE);
  } else {
    gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
    setUiBigFont();
    gfx->setCursor(28, 320);
    gfx->print(tr("forecast unavailable", "無預報資料"));
    resetUiBigFont();
  }
  present();
}

void drawSectionHeader(int y, const char* label) {
  setUiFont();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(SET_ROW_X, y);
  gfx->print(label);
  gfx->setFont(&fonts::Font2);
}

// A small distinct glyph for each app tile, drawn centred on (cx, cy).
void drawAppIcon(int cx, int cy, int kind, uint32_t color) {
  switch (kind) {
    case 0:  // calculator
      gfx->fillRoundRect(cx - 9, cy - 10, 18, 20, 3, color);
      gfx->fillRect(cx - 6, cy - 7, 12, 4, COLOR_BG);
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
          gfx->fillRect(cx - 6 + j * 5, cy - 1 + i * 4, 2, 2, COLOR_BG);
      break;
    case 1:  // pomodoro (tomato)
      gfx->fillCircle(cx, cy + 2, 10, color);
      gfx->fillRect(cx - 2, cy - 11, 4, 5, COLOR_GOOD);
      break;
    case 2:  // stopwatch
      gfx->drawCircle(cx, cy + 1, 10, color);
      gfx->drawCircle(cx, cy + 1, 9, color);
      gfx->fillRect(cx - 3, cy - 12, 6, 3, color);
      gfx->drawLine(cx, cy + 1, cx + 4, cy - 4, color);
      break;
    case 3:  // timer (hourglass-ish triangle)
      gfx->fillTriangle(cx - 9, cy - 9, cx + 9, cy - 9, cx, cy + 1, color);
      gfx->fillTriangle(cx - 9, cy + 11, cx + 9, cy + 11, cx, cy + 1, color);
      break;
    case 5:  // language (speech bubble)
      gfx->fillRoundRect(cx - 11, cy - 10, 22, 16, 5, color);
      gfx->fillTriangle(cx - 4, cy + 5, cx + 4, cy + 5, cx - 2, cy + 11, color);
      gfx->fillRect(cx - 6, cy - 6, 12, 2, COLOR_BG);
      gfx->fillRect(cx - 6, cy - 2, 8, 2, COLOR_BG);
      break;
    default:  // info
      gfx->fillCircle(cx, cy, 11, color);
      gfx->fillCircle(cx, cy - 4, 2, COLOR_BG);
      gfx->fillRect(cx - 1, cy - 1, 2, 8, COLOR_BG);
      break;
  }
}

void drawAppTile(int idx, const char* name, int iconKind, uint32_t color) {
  int x, y;
  appTileRect(idx, x, y);
  gfx->fillRoundRect(x, y, APP_W, APP_H, 8, COLOR_PANEL_2);
  gfx->drawRoundRect(x, y, APP_W, APP_H, 8, COLOR_RULE);
  drawAppIcon(x + APP_W / 2, y + 20, iconKind, color);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  setUiFont12();
  gfx->setCursor(x + (APP_W - gfx->textWidth(name)) / 2, y + 40);
  gfx->print(name);
  gfx->setFont(&fonts::Font2);
}

void drawSysButton(int col, const char* label, uint32_t color) {
  int x;
  sysBtnRect(col, x);
  gfx->fillRoundRect(x, SYS_Y, SYS_W, SYS_H, 8, COLOR_PANEL_2);
  gfx->drawRoundRect(x, SYS_Y, SYS_W, SYS_H, 8, COLOR_RULE);
  gfx->setTextColor(color, COLOR_PANEL_2);
  setUiFont12();
  gfx->setCursor(x + (SYS_W - gfx->textWidth(label)) / 2, SYS_Y + 14);
  gfx->print(label);
  gfx->setFont(&fonts::Font2);
}

void drawSettingsPage() {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();

  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  setUiFont24();
  gfx->setCursor(300 - gfx->textWidth(tr("Settings", "設定")), 20);
  gfx->print(tr("Settings", "設定"));
  gfx->setFont(&fonts::Font2);

  drawSectionHeader(66, tr("DISPLAY", "顯示"));

  // Brightness row: label + - value + .
  gfx->fillRoundRect(SET_ROW_X, BRI_ROW_Y, SET_ROW_W, SET_ROW_H, 10, COLOR_PANEL_2);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  setUiBigFont();
  gfx->setCursor(SET_ROW_X + 14, BRI_ROW_Y + 14);
  gfx->print(tr("Brightness", "亮度"));
  resetUiBigFont();
  gfx->setTextSize(2);
  gfx->fillCircle(BRI_MINUS_X + BRI_BTN_WH / 2, BRI_ROW_Y + BRI_BTN_WH / 2 + 2, 16, COLOR_PANEL);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setCursor(BRI_MINUS_X + 14, BRI_ROW_Y + 15);
  gfx->print("-");
  gfx->fillCircle(BRI_PLUS_X + BRI_BTN_WH / 2, BRI_ROW_Y + BRI_BTN_WH / 2 + 2, 16, COLOR_PANEL);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setCursor(BRI_PLUS_X + 12, BRI_ROW_Y + 15);
  gfx->print("+");
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL_2);
  gfx->setCursor(BRI_MINUS_X + BRI_BTN_WH + 6, BRI_ROW_Y + 15);
  gfx->printf("%3d", dayBrightness);

  // Night dim toggle row.
  gfx->fillRoundRect(SET_ROW_X, NIGHT_ROW_Y, SET_ROW_W, SET_ROW_H, 10, COLOR_PANEL_2);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  setUiBigFont();
  gfx->setCursor(SET_ROW_X + 14, NIGHT_ROW_Y + 14);
  gfx->print(tr("Night dim", "夜間亮度"));
  gfx->fillRoundRect(NIGHT_TGL_X, NIGHT_ROW_Y + 2, NIGHT_TGL_W, NIGHT_TGL_H, 8,
                     nightDimEnabled ? COLOR_GOOD : COLOR_PANEL);
  gfx->setTextColor(nightDimEnabled ? COLOR_BG : COLOR_MUTED, nightDimEnabled ? COLOR_GOOD : COLOR_PANEL);
  gfx->setCursor(NIGHT_TGL_X + 30, NIGHT_ROW_Y + 14);
  gfx->print(nightDimEnabled ? tr("Auto", "自動") : tr("Off", "關閉"));
  resetUiBigFont();

  // Theme row: three preview swatches, selected one gets an accent ring.
  gfx->fillRoundRect(SET_ROW_X, THEME_ROW_Y, SET_ROW_W, SET_ROW_H, 10, COLOR_PANEL_2);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  setUiBigFont();
  gfx->setCursor(SET_ROW_X + 14, THEME_ROW_Y + 14);
  gfx->print(tr("Theme", "主題"));
  resetUiBigFont();
  const uint32_t swBg[3] = {0x101215, 0x000000, 0xEEF1F5};
  const uint32_t swPanel[3] = {0x2A313B, 0x191C21, 0xFFFFFF};
  for (int i = 0; i < 3; ++i) {
    int sx = THEME_SW_X0 + i * THEME_SW_GAP;
    gfx->fillRoundRect(sx, THEME_SW_Y, THEME_SW_W, THEME_SW_H, 6, swBg[i]);
    gfx->fillRoundRect(sx + 6, THEME_SW_Y + 7, THEME_SW_W - 12, THEME_SW_H - 14, 4, swPanel[i]);
    gfx->drawRoundRect(sx, THEME_SW_Y, THEME_SW_W, THEME_SW_H, 6, COLOR_RULE);
    if ((int)uiTheme == i) {
      gfx->drawRoundRect(sx - 2, THEME_SW_Y - 2, THEME_SW_W + 4, THEME_SW_H + 4, 7, COLOR_CODEX);
      gfx->drawRoundRect(sx - 3, THEME_SW_Y - 3, THEME_SW_W + 6, THEME_SW_H + 6, 8, COLOR_CODEX);
    }
  }

  drawSectionHeader(228, tr("APPS", "應用程式"));
  drawAppTile(0, tr("Calc", "計算機"), 0, COLOR_CODEX);
  drawAppTile(1, tr("Pomodoro", "番茄鐘"), 1, COLOR_DANGER);
  drawAppTile(2, tr("Stopwatch", "碼表"), 2, COLOR_GOOD);
  drawAppTile(3, tr("Timer", "計時器"), 3, COLOR_WARN);
  drawAppTile(4, tr("System", "系統"), 4, COLOR_CLOUD);
  // Language toggle lives in the spare sixth tile; the label shows the
  // currently active language, tapping switches to the other one.
  drawAppTile(5, zhUi() ? "中文" : "English", 5, COLOR_WEATHER);

  drawSectionHeader(382, tr("SYSTEM", "系統"));
  drawSysButton(0, "Wi-Fi", COLOR_CODEX);
  drawSysButton(1, tr("Refresh", "重新整理"), COLOR_GOOD);
  drawSysButton(2, tr("Restart", "重新啟動"), COLOR_DANGER);
  present();
}

// ---------------------------------------------------------------------------
// On-device Wi-Fi entry: scan list + touch QWERTY keyboard.
// ---------------------------------------------------------------------------

void drawCurrentView();  // defined just below; used by the tap handlers here

static constexpr int SCAN_ROW_Y = 76;   // first network row
static constexpr int SCAN_ROW_H = 42;
static constexpr int SCAN_RESCAN_Y = 424;

// Keyboard geometry.
static constexpr int KEY_W = 28, KEY_H = 40, KEY_GAP = 2;
static constexpr int KB_ROW0_Y = 252, KB_ROW1_Y = 296, KB_ROW2_Y = 340, KB_ROW3_Y = 384;
static constexpr int KB_R0_X = 11;   // 10 keys
static constexpr int KB_R1_X = 26;   // 9 keys, offset
static constexpr int KB_SHIFT_X = 12, KB_SHIFT_W = 42;
static constexpr int KB_R2_X = 56;   // 7 letters after shift
static constexpr int KB_BKSP_X = 266, KB_BKSP_W = 42;
static constexpr int KB_SYM_X = 18, KB_SYM_W = 60;
static constexpr int KB_SPACE_X = 80, KB_SPACE_W = 160;
static constexpr int KB_OK_X = 242, KB_OK_W = 60;

const char* KB_ROW0 = "qwertyuiop";
const char* KB_ROW1 = "asdfghjkl";
const char* KB_ROW2 = "zxcvbnm";
const char* KB_SYM0 = "1234567890";
const char* KB_SYM1 = "@#$_&-+()";
const char* KB_SYM2 = ".,:;!?/";

char kbGlyph(char c) {
  return (kbShift && !kbSymbols && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

void drawKey(int x, int y, int w, const char* label, uint32_t bg, uint32_t fg) {
  gfx->fillRoundRect(x, y, w, KEY_H, 5, bg);
  gfx->drawRoundRect(x, y, w, KEY_H, 5, COLOR_RULE);
  gfx->setTextColor(fg, bg);
  gfx->setFont(&fonts::DejaVu18);
  gfx->setCursor(x + (w - gfx->textWidth(label)) / 2, y + 10);
  gfx->print(label);
  gfx->setFont(&fonts::Font2);
}

void drawKeyRow(const char* keys, int y, int startX) {
  for (int i = 0; keys[i]; ++i) {
    char s[2] = {kbGlyph(keys[i]), 0};
    drawKey(startX + i * (KEY_W + KEY_GAP), y, KEY_W, s, COLOR_PANEL_2, COLOR_TEXT);
  }
}

void drawWifiKeyPage() {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();

  gfx->setTextColor(COLOR_CODEX, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(300 - gfx->textWidth("Password"), 20);
  gfx->print("Password");
  gfx->setFont(&fonts::Font2);

  // SSID + typed password field.
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(20, 74);
  gfx->print("Network");
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu18);
  gfx->setCursor(20, 86);
  gfx->print(fittedText(inputSsid, 24));

  gfx->fillRoundRect(20, 120, 280, 34, 6, COLOR_PANEL_2);
  gfx->drawRoundRect(20, 120, 280, 34, 6, COLOR_CODEX);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  gfx->setCursor(28, 128);
  String shown = inputPassword.length() ? fittedText(inputPassword, 21) : String("");
  gfx->print(shown);
  // caret
  gfx->fillRect(28 + gfx->textWidth(shown) + 1, 127, 2, 18, COLOR_CODEX);
  gfx->setFont(&fonts::Font2);

  const char* r0 = kbSymbols ? KB_SYM0 : KB_ROW0;
  const char* r1 = kbSymbols ? KB_SYM1 : KB_ROW1;
  const char* r2 = kbSymbols ? KB_SYM2 : KB_ROW2;
  drawKeyRow(r0, KB_ROW0_Y, KB_R0_X);
  drawKeyRow(r1, KB_ROW1_Y, KB_R1_X);

  // Row 2: shift, letters, backspace.
  drawKey(KB_SHIFT_X, KB_ROW2_Y, KB_SHIFT_W, kbShift ? "^" : "v",
          kbShift ? COLOR_CODEX : COLOR_PANEL_2, kbShift ? COLOR_BG : COLOR_TEXT);
  drawKeyRow(r2, KB_ROW2_Y, KB_R2_X);
  drawKey(KB_BKSP_X, KB_ROW2_Y, KB_BKSP_W, "<x", COLOR_PANEL_2, COLOR_TEXT);

  // Row 3: 123/abc, space, OK.
  drawKey(KB_SYM_X, KB_ROW3_Y, KB_SYM_W, kbSymbols ? "abc" : "123", COLOR_PANEL_2, COLOR_TEXT);
  drawKey(KB_SPACE_X, KB_ROW3_Y, KB_SPACE_W, "space", COLOR_PANEL_2, COLOR_MUTED);
  drawKey(KB_OK_X, KB_ROW3_Y, KB_OK_W, "OK", COLOR_GOOD, COLOR_BG);
  present();
}

// Returns the tapped character in a letter row, or 0 if none.
char kbRowHit(const char* keys, int rowY, int startX, uint16_t x, uint16_t y) {
  if (y < rowY || y > rowY + KEY_H) return 0;
  for (int i = 0; keys[i]; ++i) {
    int kx = startX + i * (KEY_W + KEY_GAP);
    if (x >= kx && x <= kx + KEY_W) return kbGlyph(keys[i]);
  }
  return 0;
}

void connectWifi();
bool fetchDashboardState();
bool refreshDashboardConnectionNow();

void handleWifiKeyTap(uint16_t x, uint16_t y) {
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    view = View::WifiScan;
    drawCurrentView();
    return;
  }
  const char* r0 = kbSymbols ? KB_SYM0 : KB_ROW0;
  const char* r1 = kbSymbols ? KB_SYM1 : KB_ROW1;
  const char* r2 = kbSymbols ? KB_SYM2 : KB_ROW2;
  char c = kbRowHit(r0, KB_ROW0_Y, KB_R0_X, x, y);
  if (!c) c = kbRowHit(r1, KB_ROW1_Y, KB_R1_X, x, y);
  if (!c) c = kbRowHit(r2, KB_ROW2_Y, KB_R2_X, x, y);
  if (c) {
    inputPassword += c;
    drawCurrentView();
    return;
  }
  if (pointInRect(x, y, KB_SHIFT_X, KB_ROW2_Y, KB_SHIFT_W, KEY_H)) {
    kbShift = !kbShift;
  } else if (pointInRect(x, y, KB_BKSP_X, KB_ROW2_Y, KB_BKSP_W, KEY_H)) {
    if (inputPassword.length()) inputPassword.remove(inputPassword.length() - 1);
  } else if (pointInRect(x, y, KB_SYM_X, KB_ROW3_Y, KB_SYM_W, KEY_H)) {
    kbSymbols = !kbSymbols;
  } else if (pointInRect(x, y, KB_SPACE_X, KB_ROW3_Y, KB_SPACE_W, KEY_H)) {
    inputPassword += ' ';
  } else if (pointInRect(x, y, KB_OK_X, KB_ROW3_Y, KB_OK_W, KEY_H)) {
    // Save and connect.
    saveSettings(inputSsid, inputPassword, runtimeApiUrl);
    runtimeWifiSsid = inputSsid;
    runtimeWifiPassword = inputPassword;
    gfx->fillScreen(COLOR_BG);
    drawPanel(32, 190, 256, 92);
    gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
    gfx->setTextSize(2);
    gfx->setCursor(64, 222);
    gfx->print("Connecting...");
    present();
    refreshDashboardConnectionNow();
    view = View::Dashboard;
    drawCurrentView();
    return;
  } else {
    return;
  }
  drawCurrentView();
}

void drawWifiScanPage() {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();
  gfx->setTextColor(COLOR_CODEX, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(300 - gfx->textWidth("Wi-Fi"), 20);
  gfx->print("Wi-Fi");
  gfx->setFont(&fonts::Font2);

  gfx->setFont(&fonts::DejaVu18);
  if (wifiSsidCount == 0) {
    gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
    gfx->setCursor(40, 200);
    gfx->print("No networks found");
  } else {
    for (int i = 0; i < wifiSsidCount; ++i) {
      int ry = SCAN_ROW_Y + i * (SCAN_ROW_H + 2);
      gfx->fillRoundRect(20, ry, 280, SCAN_ROW_H, 6, COLOR_PANEL_2);
      gfx->drawRoundRect(20, ry, 280, SCAN_ROW_H, 6, COLOR_RULE);
      gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
      gfx->setCursor(32, ry + 11);
      gfx->print(fittedText(wifiSsids[i], 20));
    }
  }

  gfx->fillRoundRect(20, SCAN_RESCAN_Y, 280, 40, 8, COLOR_PANEL_2);
  gfx->drawRoundRect(20, SCAN_RESCAN_Y, 280, 40, 8, COLOR_RULE);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  gfx->setCursor(122, SCAN_RESCAN_Y + 11);
  gfx->print("Rescan");
  gfx->setFont(&fonts::Font2);
  present();
}

void runWifiScan() {
  // Show a scanning splash, then do a (blocking) scan and store SSIDs.
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setTextSize(2);
  gfx->setCursor(90, 220);
  gfx->print("Scanning...");
  present();

  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();
  wifiSsidCount = 0;
  for (int i = 0; i < found && wifiSsidCount < WIFI_SCAN_MAX; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < wifiSsidCount; ++j) {
      if (wifiSsids[j] == ssid) { dup = true; break; }
    }
    if (!dup) wifiSsids[wifiSsidCount++] = ssid;
  }
  WiFi.scanDelete();
}

void handleWifiScanTap(uint16_t x, uint16_t y) {
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    view = View::Settings;
    drawCurrentView();
    return;
  }
  if (pointInRect(x, y, 20, SCAN_RESCAN_Y, 280, 40)) {
    runWifiScan();
    drawWifiScanPage();
    return;
  }
  for (int i = 0; i < wifiSsidCount; ++i) {
    int ry = SCAN_ROW_Y + i * (SCAN_ROW_H + 2);
    if (pointInRect(x, y, 20, ry, 280, SCAN_ROW_H)) {
      inputSsid = wifiSsids[i];
      inputPassword = "";
      kbShift = false;
      kbSymbols = false;
      view = View::WifiKey;
      drawCurrentView();
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Calculator (touch, immediate-execution).
// ---------------------------------------------------------------------------

static constexpr int CALC_COLS = 4;
static constexpr int CALC_BTN_W = 66, CALC_BTN_H = 52, CALC_BTN_GAP = 6;
static constexpr int CALC_GRID_X = 19, CALC_GRID_Y = 168;

const char* CALC_KEYS[5][4] = {
    {"C", "+/-", "%", "/"},
    {"7", "8", "9", "x"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "+"},
    {"0", "", ".", "="},
};

bool calcIsOp(const char* k) {
  return k[1] == 0 && (k[0] == '/' || k[0] == 'x' || k[0] == '-' || k[0] == '+');
}

double calcApply(double a, char op, double b) {
  switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b != 0 ? a / b : NAN;
  }
  return b;
}

String calcFormat(double v) {
  if (isnan(v) || isinf(v)) return "Err";
  if (v == (long long)v && fabs(v) < 1e15) return String((long long)v);
  String s = String(v, 6);
  while (s.endsWith("0")) s.remove(s.length() - 1);
  if (s.endsWith(".")) s.remove(s.length() - 1);
  return s;
}

void calcInput(const char* k) {
  char c = k[0];
  if (c >= '0' && c <= '9') {
    if (calcFresh || calcDisplay == "0") {
      calcDisplay = k;
      calcFresh = false;
    } else if (calcDisplay.length() < 12) {
      calcDisplay += k;
    }
  } else if (strcmp(k, "C") == 0) {
    calcDisplay = "0";
    calcAccum = 0;
    calcOp = 0;
    calcFresh = true;
  } else if (strcmp(k, "+/-") == 0) {
    if (calcDisplay != "0") {
      if (calcDisplay.startsWith("-")) calcDisplay.remove(0, 1);
      else calcDisplay = "-" + calcDisplay;
    }
  } else if (strcmp(k, "%") == 0) {
    calcDisplay = calcFormat(calcDisplay.toDouble() / 100.0);
    calcFresh = true;
  } else if (strcmp(k, ".") == 0) {
    if (calcFresh) {
      calcDisplay = "0.";
      calcFresh = false;
    } else if (calcDisplay.indexOf('.') < 0) {
      calcDisplay += ".";
    }
  } else if (calcIsOp(k)) {
    double cur = calcDisplay.toDouble();
    if (calcOp && !calcFresh) {
      calcAccum = calcApply(calcAccum, calcOp, cur);
      calcDisplay = calcFormat(calcAccum);
    } else {
      calcAccum = cur;
    }
    calcOp = (c == 'x') ? '*' : c;
    calcFresh = true;
  } else if (strcmp(k, "=") == 0) {
    if (calcOp) {
      calcAccum = calcApply(calcAccum, calcOp, calcDisplay.toDouble());
      calcDisplay = calcFormat(calcAccum);
      calcOp = 0;
      calcFresh = true;
    }
  }
}

void drawCalcButton(int x, int y, int w, const char* label, uint32_t bg, uint32_t fg) {
  gfx->fillRoundRect(x, y, w, CALC_BTN_H, 8, bg);
  gfx->setTextColor(fg, bg);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(x + (w - gfx->textWidth(label)) / 2, y + 14);
  gfx->print(label);
  gfx->setFont(&fonts::Font2);
}

void drawCalculatorPage() {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();
  gfx->setTextColor(COLOR_CODEX, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(300 - gfx->textWidth("Calc"), 20);
  gfx->print("Calc");
  gfx->setFont(&fonts::Font2);

  // Display (right-aligned).
  gfx->fillRoundRect(20, 92, 280, 56, 8, COLOR_PANEL_2);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
  gfx->setFont(&fonts::DejaVu40);
  String d = fittedText(calcDisplay, 11);
  gfx->setCursor(292 - gfx->textWidth(d), 100);
  gfx->print(d);
  gfx->setFont(&fonts::Font2);

  for (int r = 0; r < 5; ++r) {
    int by = CALC_GRID_Y + r * (CALC_BTN_H + CALC_BTN_GAP);
    for (int c = 0; c < CALC_COLS; ++c) {
      const char* k = CALC_KEYS[r][c];
      if (k[0] == 0) continue;  // filler (0 spans two cells)
      int bx = CALC_GRID_X + c * (CALC_BTN_W + CALC_BTN_GAP);
      int w = CALC_BTN_W;
      if (r == 4 && c == 0) w = CALC_BTN_W * 2 + CALC_BTN_GAP;  // wide "0"
      uint32_t bg = COLOR_PANEL_2, fg = COLOR_TEXT;
      if (calcIsOp(k) || k[0] == '=') { bg = COLOR_CODEX; fg = COLOR_BG; }
      else if (r == 0) { bg = COLOR_RULE; fg = COLOR_TEXT; }
      drawCalcButton(bx, by, w, k, bg, fg);
    }
  }
  present();
}

void handleCalculatorTap(uint16_t x, uint16_t y) {
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    view = View::Settings;
    drawCurrentView();
    return;
  }
  for (int r = 0; r < 5; ++r) {
    int by = CALC_GRID_Y + r * (CALC_BTN_H + CALC_BTN_GAP);
    if (y < by || y > by + CALC_BTN_H) continue;
    for (int c = 0; c < CALC_COLS; ++c) {
      const char* k = CALC_KEYS[r][c];
      if (k[0] == 0) continue;
      int bx = CALC_GRID_X + c * (CALC_BTN_W + CALC_BTN_GAP);
      int w = (r == 4 && c == 0) ? CALC_BTN_W * 2 + CALC_BTN_GAP : CALC_BTN_W;
      if (x >= bx && x <= bx + w) {
        calcInput(k);
        drawCurrentView();
        return;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Pomodoro / Stopwatch / Timer / System info.
// ---------------------------------------------------------------------------

// Two big control buttons at the bottom of a tool page.
static constexpr int CTRL_Y = 384, CTRL_H = 56;
static constexpr int CTRL_L_X = 24, CTRL_R_X = 168, CTRL_W = 128;

void drawCtrlButton(int x, const char* label, uint32_t bg, uint32_t fg) {
  gfx->fillRoundRect(x, CTRL_Y, CTRL_W, CTRL_H, 10, bg);
  gfx->setTextColor(fg, bg);
  gfx->setFont(&fonts::DejaVu18);
  gfx->setCursor(x + (CTRL_W - gfx->textWidth(label)) / 2, CTRL_Y + 18);
  gfx->print(label);
  gfx->setFont(&fonts::Font2);
}

void drawToolHeader(const char* title, uint32_t accent) {
  gfx->fillScreen(COLOR_BG);
  drawPanel(8, 8, 304, 464);
  drawBackButton();
  gfx->setTextColor(accent, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu24);
  gfx->setCursor(300 - gfx->textWidth(title), 20);
  gfx->print(title);
  gfx->setFont(&fonts::Font2);
}

// Big centred clock string.
void drawBigClock(const String& s, uint32_t color, int y) {
  gfx->setTextColor(color, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu72);
  gfx->setCursor((320 - gfx->textWidth(s)) / 2, y);
  gfx->print(s);
  gfx->setFont(&fonts::Font2);
}

int pomoRemaining() {
  if (pomoRunning) return (int)((long)(pomoEndMs - millis()) / 1000);
  return pomoRemainSec;
}

// Duration adjuster rows (shown only while the pomodoro is idle).
static constexpr int POMO_ADJ_MINUS_X = 150, POMO_ADJ_PLUS_X = 252, POMO_ADJ_W = 40, POMO_ADJ_H = 40;
static constexpr int POMO_FOCUS_Y = 250, POMO_BREAK_Y = 304;

void drawPomoAdjuster(int y, const char* label, int minutes, uint32_t accent) {
  gfx->setFont(&fonts::DejaVu18);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setCursor(28, y + 11);
  gfx->print(label);
  gfx->fillRoundRect(POMO_ADJ_MINUS_X, y, POMO_ADJ_W, POMO_ADJ_H, 8, COLOR_PANEL_2);
  gfx->setCursor(POMO_ADJ_MINUS_X + 15, y + 10);
  gfx->print("-");
  gfx->fillRoundRect(POMO_ADJ_PLUS_X, y, POMO_ADJ_W, POMO_ADJ_H, 8, COLOR_PANEL_2);
  gfx->setCursor(POMO_ADJ_PLUS_X + 13, y + 10);
  gfx->print("+");
  gfx->setTextColor(accent, COLOR_PANEL);
  char v[8];
  snprintf(v, sizeof(v), "%dm", minutes);
  gfx->setCursor(206 - gfx->textWidth(v) / 2, y + 11);
  gfx->print(v);
  gfx->setFont(&fonts::Font2);
}

void drawPomodoroPage() {
  drawToolHeader("Pomodoro", COLOR_DANGER);
  bool work = pomoWork;
  gfx->setTextColor(work ? COLOR_DANGER : COLOR_GOOD, COLOR_PANEL);
  gfx->setFont(&fonts::DejaVu18);
  const char* phase = work ? "FOCUS" : "BREAK";
  gfx->setCursor((320 - gfx->textWidth(phase)) / 2, 108);
  gfx->print(phase);
  gfx->setFont(&fonts::Font2);

  drawBigClock(fmtClock(pomoRemaining()), COLOR_TEXT, 168);

  if (pomoRunning) {
    gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
    gfx->setFont(&fonts::DejaVu18);
    char b[24];
    snprintf(b, sizeof(b), "Done today: %d", pomoDoneCount);
    gfx->setCursor((320 - gfx->textWidth(b)) / 2, 292);
    gfx->print(b);
    gfx->setFont(&fonts::Font2);
  } else {
    drawPomoAdjuster(POMO_FOCUS_Y, "Focus", pomoWorkMin, COLOR_DANGER);
    drawPomoAdjuster(POMO_BREAK_Y, "Break", pomoBreakMin, COLOR_GOOD);
  }

  drawCtrlButton(CTRL_L_X, pomoRunning ? "Pause" : "Start",
                 pomoRunning ? COLOR_WARN : COLOR_GOOD, COLOR_BG);
  drawCtrlButton(CTRL_R_X, "Reset", COLOR_PANEL_2, COLOR_TEXT);
  present();
}

void drawStopwatchPage() {
  drawToolHeader("Stopwatch", COLOR_GOOD);
  unsigned long ms = swAccumMs + (swRunning ? millis() - swStartMs : 0);
  drawBigClock(fmtClock(ms / 1000), COLOR_TEXT, 190);

  drawCtrlButton(CTRL_L_X, swRunning ? "Stop" : "Start",
                 swRunning ? COLOR_DANGER : COLOR_GOOD, COLOR_BG);
  drawCtrlButton(CTRL_R_X, "Reset", COLOR_PANEL_2, COLOR_TEXT);
  present();
}

int timerRemaining() {
  if (tmRunning) return (int)((long)(tmEndMs - millis()) / 1000);
  return tmRemainSec;
}

void drawTimerPage() {
  drawToolHeader("Timer", COLOR_WARN);
  drawBigClock(fmtClock(timerRemaining()), tmRunning ? COLOR_TEXT : COLOR_WARN, 150);

  // -1m / +1m adjusters (only when not running).
  if (!tmRunning) {
    gfx->setFont(&fonts::DejaVu18);
    gfx->fillRoundRect(40, 250, 100, 46, 10, COLOR_PANEL_2);
    gfx->setTextColor(COLOR_TEXT, COLOR_PANEL_2);
    gfx->setCursor(70, 262);
    gfx->print("-1m");
    gfx->fillRoundRect(180, 250, 100, 46, 10, COLOR_PANEL_2);
    gfx->setCursor(210, 262);
    gfx->print("+1m");
    gfx->setFont(&fonts::Font2);
  }

  drawCtrlButton(CTRL_L_X, tmRunning ? "Pause" : "Start",
                 tmRunning ? COLOR_WARN : COLOR_GOOD, COLOR_BG);
  drawCtrlButton(CTRL_R_X, "Reset", COLOR_PANEL_2, COLOR_TEXT);
  present();
}

void drawInfoRow(int y, const char* label, const String& value) {
  gfx->setFont(&fonts::DejaVu18);
  gfx->setTextColor(COLOR_MUTED, COLOR_PANEL);
  gfx->setCursor(24, y);
  gfx->print(label);
  gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
  gfx->setCursor(300 - gfx->textWidth(value), y);
  gfx->print(value);
  gfx->setFont(&fonts::Font2);
}

void drawSysInfoPage() {
  drawToolHeader("System", COLOR_CLOUD);
  bool up = WiFi.status() == WL_CONNECTED;
  drawInfoRow(96, "Wi-Fi", up ? "connected" : "offline");
  drawInfoRow(134, "IP", up ? WiFi.localIP().toString() : String("-"));
  drawInfoRow(172, "Signal", up ? String(WiFi.RSSI()) + " dBm" : String("-"));
  drawInfoRow(210, "SSID", up ? WiFi.SSID() : String("-"));
  drawInfoRow(248, "Uptime", fmtClock(millis() / 1000));
  drawInfoRow(286, "Free RAM", String(ESP.getFreeHeap() / 1024) + " KB");
  drawInfoRow(324, "PSRAM", String(ESP.getFreePsram() / 1024) + " KB");
  drawInfoRow(362, "Data", state.source);
  present();
}

void drawCurrentView() {
  gfx->setFont(&fonts::Font2);  // pages below assume the bitmap font
  gfx->setTextSize(1);
  switch (view) {
    case View::Dashboard: drawDashboard(); break;
    case View::DetailCodex: drawDetailPage("Codex", COLOR_CODEX, state.codex5h, state.codexWeekly, state.codexChart); break;
    case View::DetailClaude: drawDetailPage("Claude", COLOR_CLAUDE, state.claude5h, state.claudeWeekly, state.claudeChart); break;
    case View::Weather: drawWeatherPage(); break;
    case View::Settings: drawSettingsPage(); break;
    case View::WifiScan: drawWifiScanPage(); break;
    case View::WifiKey: drawWifiKeyPage(); break;
    case View::Calculator: drawCalculatorPage(); break;
    case View::Pomodoro: drawPomodoroPage(); break;
    case View::Stopwatch: drawStopwatchPage(); break;
    case View::Timer: drawTimerPage(); break;
    case View::SysInfo: drawSysInfoPage(); break;
  }
}

// Background ticks for pomodoro + timer (run even off-screen); fire the LED
// alert when a phase completes. Returns true if a redraw is warranted.
void toolsTick() {
  if (pomoRunning && millis() >= pomoEndMs) {
    pomoWork = !pomoWork;
    if (pomoWork) pomoDoneCount++;  // a break just finished -> a full cycle done
    pomoEndMs = millis() + (unsigned long)(pomoWork ? pomoWorkMin : pomoBreakMin) * 60UL * 1000UL;
    alertUntilMs = millis() + 8000;
  }
  if (tmRunning && millis() >= tmEndMs) {
    tmRunning = false;
    tmRemainSec = 0;
    alertUntilMs = millis() + 8000;
  }
}

void handlePomodoroTap(uint16_t x, uint16_t y) {
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    view = View::Settings;
  } else if (pointInRect(x, y, CTRL_L_X, CTRL_Y, CTRL_W, CTRL_H)) {
    if (pomoRunning) {
      pomoRemainSec = pomoRemaining();
      pomoRunning = false;
    } else {
      pomoEndMs = millis() + (unsigned long)pomoRemainSec * 1000UL;
      pomoRunning = true;
    }
  } else if (pointInRect(x, y, CTRL_R_X, CTRL_Y, CTRL_W, CTRL_H)) {
    pomoRunning = false;
    pomoWork = true;
    pomoRemainSec = pomoWorkMin * 60;
  } else if (!pomoRunning && pointInRect(x, y, POMO_ADJ_MINUS_X, POMO_FOCUS_Y, POMO_ADJ_W, POMO_ADJ_H)) {
    if (pomoWorkMin > 5) pomoWorkMin -= 5;
    if (pomoWork) pomoRemainSec = pomoWorkMin * 60;
    saveDisplaySettings();
  } else if (!pomoRunning && pointInRect(x, y, POMO_ADJ_PLUS_X, POMO_FOCUS_Y, POMO_ADJ_W, POMO_ADJ_H)) {
    if (pomoWorkMin < 90) pomoWorkMin += 5;
    if (pomoWork) pomoRemainSec = pomoWorkMin * 60;
    saveDisplaySettings();
  } else if (!pomoRunning && pointInRect(x, y, POMO_ADJ_MINUS_X, POMO_BREAK_Y, POMO_ADJ_W, POMO_ADJ_H)) {
    if (pomoBreakMin > 1) pomoBreakMin -= 1;
    saveDisplaySettings();
  } else if (!pomoRunning && pointInRect(x, y, POMO_ADJ_PLUS_X, POMO_BREAK_Y, POMO_ADJ_W, POMO_ADJ_H)) {
    if (pomoBreakMin < 30) pomoBreakMin += 1;
    saveDisplaySettings();
  } else {
    return;
  }
  drawCurrentView();
}

void handleStopwatchTap(uint16_t x, uint16_t y) {
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    view = View::Settings;
  } else if (pointInRect(x, y, CTRL_L_X, CTRL_Y, CTRL_W, CTRL_H)) {
    if (swRunning) {
      swAccumMs += millis() - swStartMs;
      swRunning = false;
    } else {
      swStartMs = millis();
      swRunning = true;
    }
  } else if (pointInRect(x, y, CTRL_R_X, CTRL_Y, CTRL_W, CTRL_H)) {
    swRunning = false;
    swAccumMs = 0;
  } else {
    return;
  }
  drawCurrentView();
}

void handleTimerTap(uint16_t x, uint16_t y) {
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    view = View::Settings;
  } else if (!tmRunning && pointInRect(x, y, 40, 250, 100, 46)) {
    if (tmRemainSec > 60) {
      tmRemainSec -= 60;
      tmSetSec = tmRemainSec;
    }
  } else if (!tmRunning && pointInRect(x, y, 180, 250, 100, 46)) {
    if (tmRemainSec < 99 * 60) {
      tmRemainSec += 60;
      tmSetSec = tmRemainSec;
    }
  } else if (pointInRect(x, y, CTRL_L_X, CTRL_Y, CTRL_W, CTRL_H)) {
    if (tmRunning) {
      tmRemainSec = timerRemaining();
      tmRunning = false;
    } else if (tmRemainSec > 0) {
      tmEndMs = millis() + (unsigned long)tmRemainSec * 1000UL;
      tmRunning = true;
    }
  } else if (pointInRect(x, y, CTRL_R_X, CTRL_Y, CTRL_W, CTRL_H)) {
    tmRunning = false;
    tmRemainSec = tmSetSec;
  } else {
    return;
  }
  drawCurrentView();
}

void handleSettingsTap(uint16_t x, uint16_t y) {
  static const View APP_VIEWS[5] = {View::Calculator, View::Pomodoro, View::Stopwatch,
                                    View::Timer, View::SysInfo};
  if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
    refreshDashboardConnectionNow();
    view = View::Dashboard;
  } else if (pointInRect(x, y, BRI_MINUS_X, BRI_ROW_Y, BRI_BTN_WH, BRI_BTN_WH)) {
    dayBrightness = dayBrightness > 40 ? dayBrightness - 20 : 20;
    currentBrightness = 0;
    applyAutoBrightness();
    saveDisplaySettings();
  } else if (pointInRect(x, y, BRI_PLUS_X, BRI_ROW_Y, BRI_BTN_WH, BRI_BTN_WH)) {
    dayBrightness = dayBrightness < 235 ? dayBrightness + 20 : 255;
    currentBrightness = 0;
    applyAutoBrightness();
    saveDisplaySettings();
  } else if (pointInRect(x, y, NIGHT_TGL_X, NIGHT_ROW_Y, NIGHT_TGL_W, NIGHT_TGL_H)) {
    nightDimEnabled = !nightDimEnabled;
    currentBrightness = 0;
    applyAutoBrightness();
    saveDisplaySettings();
  } else if (y >= THEME_SW_Y - 3 && y <= THEME_SW_Y + THEME_SW_H + 3 &&
             x >= THEME_SW_X0 && x < THEME_SW_X0 + 3 * THEME_SW_GAP) {
    int i = (x - THEME_SW_X0) / THEME_SW_GAP;
    if (i >= 0 && i <= 2) {
      applyTheme((Theme)i);
      saveDisplaySettings();
    }
  } else if (pointInRect(x, y, APP_X0, SYS_Y, SYS_W, SYS_H)) {  // Wi-Fi
    view = View::WifiScan;
    runWifiScan();
    drawCurrentView();
    return;
  } else if (pointInRect(x, y, APP_X0 + (APP_W + APP_GAP), SYS_Y, SYS_W, SYS_H)) {  // Refresh
    refreshDashboardConnectionNow();
    view = View::Dashboard;
  } else if (pointInRect(x, y, APP_X0 + 2 * (APP_W + APP_GAP), SYS_Y, SYS_W, SYS_H)) {  // Restart
    ESP.restart();
  } else {
    for (int i = 0; i < 5; ++i) {
      int ax, ay;
      appTileRect(i, ax, ay);
      if (pointInRect(x, y, ax, ay, APP_W, APP_H)) {
        view = APP_VIEWS[i];
        drawCurrentView();
        return;
      }
    }
    // Sixth tile: language toggle.
    int lx, ly;
    appTileRect(5, lx, ly);
    if (pointInRect(x, y, lx, ly, APP_W, APP_H)) {
      uiLang = zhUi() ? Lang::EN : Lang::ZH;
      saveDisplaySettings();
      drawCurrentView();
    }
    return;
  }
  drawCurrentView();
}

void handleTap(uint16_t x, uint16_t y) {
  switch (view) {
    case View::Dashboard:
      if (pointInRect(x, y, 8, 8, 304, 224)) view = View::Weather;
      else if (pointInRect(x, y, 8, 248, 146, 224)) view = View::DetailCodex;
      else if (pointInRect(x, y, 166, 248, 146, 224)) view = View::DetailClaude;
      else return;
      drawCurrentView();
      break;
    case View::DetailCodex:
    case View::DetailClaude:
    case View::Weather:
      if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
        view = View::Dashboard;
        drawCurrentView();
      }
      break;
    case View::Settings:
      handleSettingsTap(x, y);
      break;
    case View::WifiScan:
      handleWifiScanTap(x, y);
      break;
    case View::WifiKey:
      handleWifiKeyTap(x, y);
      break;
    case View::Calculator:
      handleCalculatorTap(x, y);
      break;
    case View::Pomodoro:
      handlePomodoroTap(x, y);
      break;
    case View::Stopwatch:
      handleStopwatchTap(x, y);
      break;
    case View::Timer:
      handleTimerTap(x, y);
      break;
    case View::SysInfo:
      if (pointInRect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
        view = View::Settings;
        drawCurrentView();
      }
      break;
  }
}

// Long-press on the dashboard opens the on-device settings menu.
void handleLongPress() {
  if (view == View::Dashboard) {
    view = View::Settings;
    drawCurrentView();
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long serialStart = millis();
  while (!Serial && millis() - serialStart < 2000) {
    delay(10);
  }
  Serial.println("AI usage weather dashboard boot");
  loadSettings();
  Serial.printf("Stored Wi-Fi SSID: %s\n", runtimeWifiSsid.length() ? runtimeWifiSsid.c_str() : "(blank)");
  Serial.printf("Stored API URL: %s\n", runtimeApiUrl.length() ? runtimeApiUrl.c_str() : "(blank)");

  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_FREQ);
  Serial.printf("XL9555 init: %s\n", boardInitIoExpander() ? "ok" : "failed");
  Serial.printf("LCD hardware reset: %s\n", boardResetLcd() ? "ok" : "failed");
  Serial.printf("Touch hardware reset: %s\n", boardResetTouch() ? "ok" : "failed");

  Serial.println("LCD init start");
  lcd.init();
  Serial.println("LCD init done");
  Serial.printf("Touch controller: %s\n", lcd.touch() ? "configured" : "missing");
  lcd.setRotation(0);
  lcd.setBrightness(BRIGHTNESS_DAY);
  Serial.printf("LCD backlight: %s\n", boardSetBacklight(true) ? "ok" : "failed");

  // Allocate the PSRAM back buffer for flicker-free redraws. If PSRAM is
  // exhausted, keep drawing straight to the panel (gfx stays &lcd).
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (canvas.createSprite(SCREEN_W, SCREEN_H)) {
    gfx = &canvas;
    useCanvas = true;
    Serial.println("PSRAM back buffer: ok");
  } else {
    Serial.println("PSRAM back buffer: failed; drawing direct to panel");
  }
  gfx->setFont(&fonts::Font2);
  gfx->setTextWrap(false);

#ifdef LCD_SMOKE_TEST
  Serial.println("LCD smoke test mode");
  drawSmokeFrame(smokeIndex);
  lastSmokeMs = millis();
  return;
#endif

  drawDashboard();
  refreshDashboardConnectionNow();
  drawDashboard();
}

void loop() {
#ifdef LCD_SMOKE_TEST
  if (millis() - lastSmokeMs >= 1000) {
    smokeIndex = (smokeIndex + 1) % (sizeof(SMOKE_COLORS) / sizeof(SMOKE_COLORS[0]));
    Serial.printf("LCD smoke frame: %s\n", SMOKE_NAMES[smokeIndex]);
    drawSmokeFrame(smokeIndex);
    lastSmokeMs = millis();
  }
  delay(20);
  return;
#endif

  if (setupMode) {
    dnsServer.processNextRequest();
    setupServer.handleClient();
    handleSetupTouch();
    delay(10);
    return;
  }

  handleTouch();
  toolsTick();

  if (millis() - lastFetchMs >= DASHBOARD_REFRESH_MS) {
    maybeReconnectWifi();
    refreshDashboardConnectionNow();
  }

  // Render tick: advance animation, refresh the current page, apply night
  // dimming, and flash the LED on low quota or a finished pomodoro/timer.
  if (millis() - lastRenderMs >= RENDER_INTERVAL_MS) {
    animationFrame++;
    applyAutoBrightness();
    bool alerting = anyQuotaCritical() || millis() < alertUntilMs;
    boardSetLed(alerting && animationFrame % 2 == 0);
    drawCurrentView();
    lastRenderMs = millis();
  }
  delay(20);
}
