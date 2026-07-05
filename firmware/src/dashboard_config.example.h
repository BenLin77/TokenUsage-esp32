#pragma once

// Copy this file to src/dashboard_config.h and fill in your local values.
// Leaving these blank keeps the firmware in offline demo mode.

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Optional local collector endpoint. The firmware expects JSON documented in README.md.
// Example: "http://192.168.1.20:8080/dashboard.json"
#define DASHBOARD_API_URL ""

#define DASHBOARD_CITY "Taipei"
#define DASHBOARD_REFRESH_MS 120000UL

// --- Optional overrides (safe to leave commented; defaults shown) -----------
// The device clock uses a POSIX TZ string. Default is Taipei (UTC+8, no DST).
// Examples: Japan "JST-9", US Eastern "EST5EDT,M3.2.0,M11.1.0".
// #define DASHBOARD_CLOCK_TZ "CST-8"
// #define DASHBOARD_NTP_1 "pool.ntp.org"
// #define DASHBOARD_NTP_2 "time.google.com"

// First-boot Wi-Fi setup hotspot (the AP name/password shown on screen when no
// Wi-Fi is configured). The password must be >= 8 chars.
// #define DASHBOARD_SETUP_AP_SSID "ESP32-Dashboard-Setup"
// #define DASHBOARD_SETUP_AP_PASSWORD "esp32setup"
