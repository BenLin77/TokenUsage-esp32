# AGENTS.md — ESP32-S3 AI Usage Weather Dashboard

Architecture and change guide for AI agents (and humans) working on this repo.
Keep this file the single source of truth; `CLAUDE.md` just points here.

## What this is

Firmware + a small backend collector for a 3.5-inch ESP32-S3 capacitive-touch
dashboard. The device shows Taipei weather, a live clock, and remaining Claude /
Codex usage quota. The ESP32 is a **thin client**: it fetches one JSON document
over HTTP and renders it. All the real data gathering (weather APIs, quota
scraping) happens off-device in a Python collector on a small server (VPS).

The repo has two sibling folders — `firmware/` (the ESP32 build) and `server/`
(the collector + deployment) — plus this doc set at the root:

```
  ┌──────────────┐     builds       ┌────────────────────────┐    HTTP GET     ┌───────────────┐
  │ server (VPS) │ ───────────────▶ │ /var/www/.../           │ ◀────────────── │ ESP32-S3      │
  │ collector.py │  every 1 min     │ dashboard.json (nginx)  │   every 60 s    │ firmware      │
  └──────────────┘                  └────────────────────────┘                 └───────────────┘
   Codex app-server + ccusage + CWA/open-meteo                                   renders to LCD
```

## Repository map

| Path | Responsibility |
|------|----------------|
| `firmware/platformio.ini` | Build envs. Default `esp32-s3-n16r8-dashboard`; `lcd-smoke-*` envs build a panel test with `-DLCD_SMOKE_TEST` (+ optional `-DLCD_PANEL_ILI9488/ILI9486`). Build with `pio run -d firmware`. |
| `firmware/boards/esp32-s3-n16r8-dashboard.json` | Local PlatformIO board manifest (16MB flash + PSRAM). |
| `firmware/src/board_config.h` | **Hardware layer.** LovyanGFX panel/bus/touch pin map, XL9555 I/O-expander helpers (LCD/touch reset, backlight, LED). `DashboardDisplay` class + panel selection macros. |
| `firmware/src/main.cpp` | **All app logic** (see pipeline below). |
| `firmware/src/dashboard_config.h` | Local, git-ignored defaults: Wi-Fi creds, API URL, city, refresh, timezone, setup hotspot. Contains real secrets — do not commit. |
| `firmware/src/dashboard_config.example.h` | Template for the above (blank values → demo mode). |
| `server/dashboard_collector.py` | Builds `dashboard.json` from Codex app-server, `ccusage`, and weather (CWA → open-meteo fallback). Run by a systemd timer. |
| `server/test_dashboard_collector.py` | Collector regression tests (`python3 -m unittest server.test_dashboard_collector`). |
| `server/mock_dashboard_server.py` | Local HTTP endpoint for testing firmware without the server. |
| `server/{esp32-dashboard.service,esp32-dashboard.timer,nginx-esp32-dashboard.conf,.env.example}` | systemd units + nginx config + env template deployed on the server. |
| `server/dashboard.sample.json` | Canonical example payload = the API contract. |
| `慧勤智远 …/` | Vendor package (schematics, manuals, zipped example code). Reference only; the pin map in `board_config.h` is derived from it. |

## Firmware render pipeline (`firmware/src/main.cpp`)

Read top-to-bottom; it is organized in this order:

1. **Constants** — colors (`COLOR_*`), layout coords (`SETUP_*`), timing
   (`RENDER_INTERVAL_MS`, `RECONNECT_INTERVAL_MS`), clock/brightness config
   (`CLOCK_TZ`, `NTP_SERVER_*`, `BRIGHTNESS_DAY/NIGHT`, `NIGHT_*_HOUR`).
2. **State structs** — `WeatherState`, `QuotaState`, `DashboardState state`
   (the single global model everything draws from).
3. **Rendering target** — `DashboardDisplay lcd` is the physical panel.
   `LGFX_Sprite canvas` is a 320×480 PSRAM back buffer. **All drawing goes
   through `gfx` (`lgfx::LGFXBase*`)**, which points at `canvas` when the sprite
   allocates (`useCanvas == true`) and falls back to `&lcd` otherwise.
   `present()` pushes the buffer to the panel — call it at the end of any
   full-screen draw. Device-only calls (`init`, `setRotation`, `setBrightness`,
   `touch`, `getTouch`) stay on `lcd`, never `gfx`.
4. **Draw helpers** — `drawPanel`, `drawText`, weather icon primitives
   (`drawSun`/`drawCloud`/`drawRain`/…), `drawWeatherIcon`, `drawUsageBar`,
   `drawQuotaBlock`, mascots (`drawClaudeMascot`/`drawCodexMascot`),
   `drawAgentTile`, `drawWeatherTile`, `drawDashboard`.
   Animation is driven by the global `animationFrame` (incremented each render
   tick); helpers use `animationFrame % N` for blink/motion phase.
5. **Setup portal** — `startWifiSetupPortal` (SoftAP `ESP32-Dashboard-Setup` +
   captive DNS + `WebServer` on `192.168.4.1`), HTML handlers, and
   `returnToDashboardFromSetup`. Entered by a ~1.2 s long-press (`handleTouch`).
6. **Data layer** — `fetchDashboardState` (HTTP GET → `ArduinoJson` → `state`),
   `applyQuota`, `parseWeatherKind`. Sets `state.online` + `lastSuccessMs`.
7. **Network helpers** — `connectWifi` (blocking initial connect + `startNtp`),
   `maybeReconnectWifi` (loop-driven recovery), `applyAutoBrightness`.
8. **`setup()` / `loop()`** — see below.

### Main loop cadence

```
loop():
  if setupMode: service DNS + web server + setup touch; return
  handleTouch()                                  # long-press → setup portal
  every DASHBOARD_REFRESH_MS (60 s):             # from dashboard_config.h
      maybeReconnectWifi(); fetchDashboardState()
  every RENDER_INTERVAL_MS (1 s):
      animationFrame++; applyAutoBrightness(); drawDashboard()   # → present()
```

Fetch and render are **decoupled**: the device fetches every 1 min, the server
publishes every 1 min, and the screen (clock, animation, freshness age)
refreshes every 1 s. Double buffering makes the 1 s full repaint flicker-free.

## API contract

The firmware consumes one JSON document (see `server/dashboard.sample.json` and
the "API Payload" section of `README.md`). Shape:

```json
{
  "weather": { "city","temp_c","condition","label","rain_pct","rain_mm",
               "is_raining","rain_alert","weekday","day","month","year","date",
               "hourly": [ {"t":"14","temp":34,"rain":45}, ... up to 6 ] },
  "claude":  { "h5": {"used_pct","reset","tokens","cost_twd"}, "weekly": {…} },
  "codex":   { "h5": {"used_pct","reset","tokens","cost_twd"}, "weekly": {…} },
  "meta":    { "source","generated_at","timezone" }
}
```

`tokens` + `cost_twd` (API-equivalent NT$) feed the per-agent **detail page**;
`weather.hourly` feeds the **weather page** strip. Both are optional — the
firmware shows the dashboard fine without them.

## Touch pages (firmware)

`enum View { Dashboard, DetailCodex, DetailClaude, Weather, Settings }` +
`drawCurrentView()`. Tap a tile → its detail page (big bars + tokens + ~NT$);
tap the weather tile → weather page (hourly strip); long-press → on-device
Settings (brightness ∓, night dim Auto/Off, refresh, wi-fi setup, restart;
persisted via `saveDisplaySettings`). Sub-pages return via the top-left `< Back`.
Navigation lives in `handleTap` / `handleLongPress`; the Wi-Fi portal is still
separate (`setupMode`, its own web server). Low quota (<10% remaining) blinks
XL9555 LED1 (`boardSetLed`) — no on-screen border.

- `used_pct` is *used*; the UI shows `100 - used_pct` as remaining.
- `condition` keywords → `parseWeatherKind()` in `main.cpp` and
  `condition_for_weather_code()` / `cwa_condition_from_text()` in the collector.
  **Keep these in sync** when adding a weather type.
- `reset` is a human string (`"14:30"`, `"Mon"`, `"5h"`, `"roll"`). The
  collector's `reset_countdown()` rewrites clock/weekday values into
  time-remaining (`"2h10m"`); the firmware prints it verbatim.

## Build / flash / test

```bash
.venv/bin/pio run -d firmware                       # build default env
.venv/bin/pio run -d firmware -t upload             # flash (upload_port /dev/ttyACM0)
.venv/bin/pio device monitor -d firmware            # serial @ 115200

.venv/bin/pio run -d firmware -e lcd-smoke-st7796   # panel bring-up test (color bars)
python3 server/mock_dashboard_server.py --host 0.0.0.0 --port 8080   # fake API
```

Set `DASHBOARD_API_URL` to a LAN IP (not `localhost`) when pointing at the mock.
There is no hardware in CI: **a clean `pio run -d firmware` is the verification bar** for
firmware changes. Always build both a normal env and one `lcd-smoke-*` env after
touching `board_config.h` or the render target.

## Conventions / gotchas

- **Layout is absolute-positioned** (hand-tuned x/y in a 320×480 space). When
  adding UI, check the coordinate map in `drawWeatherTile` / `drawAgentTile`
  first; the weather panel is `8,8 → 312,232`, agent tiles start at `y=248`.
- **`fittedText(str, maxChars)`** truncates with a `~` — use it for any
  server-supplied string so long values don't overflow their box.
- **Colors are 24-bit `0xRRGGBB`** literals passed straight to LovyanGFX.
- **Never call drawing on `lcd` directly** in app code — use `gfx` so the back
  buffer stays authoritative, then `present()`.
- **Secrets**: `firmware/src/dashboard_config.h` holds a real SSID/password. If this repo
  ever gains a public git remote, add it to `.gitignore` and rotate creds.
- **Adding a tunable**: prefer a `static constexpr` near the top of `main.cpp`,
  or a `#define` in `dashboard_config*.h` if it's a per-deployment setting.

## Collector notes (`server/dashboard_collector.py`)

- **Codex quota is authoritative**: `codex_rate_limits_from_app_server()` calls
  `codex app-server --stdio` and sends JSON-RPC `account/rateLimits/read`, so
  the collector reads the current Codex CLI's real `usedPercent` + reset epoch
  without depending on a background app-server daemon or TUI side effect.
  `codex_rate_limits_from_logs()` still reads still-active windows from
  `~/.codex/logs_2.sqlite` as a compatibility fallback. `primary` = 5h window,
  `secondary` = weekly. Both snake_case (`reset_at`) and camelCase (`resetsAt`)
  forms are handled. Never derive quota percentage from `ccusage` token totals:
  missing/auth-blocked windows are emitted as `status: "unavailable"` without
  `used_pct`, while confirmed exhaustion is `used_pct: 100`.
- **Claude quota** is scraped from the Claude `/status` Usage screen via `tmux`
  (`claude_usage_from_tui`). A successful read is persisted for
  `DASH_CLAUDE_QUOTA_CACHE_TTL` seconds (default 900) so an intermittent TUI
  failure can use a bounded last-known-good value; after that the quota is
  `status: "unavailable"` without `used_pct`. `ccusage` is only for local token
  and cost details — never derive account quota percentage from those totals.
  Claude has no equivalent authoritative file, so its reset is whatever Claude
  itself reports (its 5h block resets at a fixed clock time, not "5h from now").
- Reset strings: codex windows are humanized from the reset epoch
  (`humanize_delta`, e.g. `4h7m`, `6d1h`); other sources may return clock/weekday
  strings that `reset_countdown()` rewrites. Humanized strings pass through
  untouched.
- Weather location: explicit `DASHBOARD_LAT`/`DASHBOARD_LON` wins for accuracy.
  If those are blank and `DASHBOARD_AUTO_LOCATION=1`, the collector can use the
  latest nginx `ESP32HTTPClient` requester IP for city-level coordinates.
- Weather: CWA (needs `CWA_API_KEY`) → open-meteo → static fallback, cached
  `WEATHER_CACHE_TTL` seconds. Open-Meteo hourly forecast is always used as a
  near-term rain overlay at the selected coordinates: the dashboard `rain_pct`
  is promoted to the max probability in the current hour + next
  `DASHBOARD_RAIN_LOOKAHEAD_HOURS` hours (default 0). `rain_alert` fires only on
  observed rain (`rain_mm > 0`, red drop) or an hourly point whose probability
  is at least `RAIN_ALERT_PCT` (default 90) **and whose same-hour expected rain**
  is at least `RAIN_ALERT_PRECIP_MM` (default 1.5 mm, amber drop). Forecast
  alerts do not rewrite the main weather icon, and hourly data without expected
  precipitation cannot alert. The base rule is computed in
  `normalize_weather()`, then `apply_near_term_rain_forecast()` adds only the
  corroborated near-term amber result.
- CWA forecast granularity: by default the collector reads the **county** (縣市)
  forecast `F-C0032-001` for `DASHBOARD_CWA_LOCATION`. Set `DASHBOARD_CWA_TOWN`
  (e.g. `中和區`) to instead read the district-accurate **township** (鄉鎮)
  forecast — dataset `DASHBOARD_CWA_TOWNSHIP_DATASET` (default `F-D0047-069` =
  新北市, one id per county). The county phrase covers a whole city for hours, so
  a summer `午後雷陣雨` wording paints rain on a dry district; the township path
  fixes the *location*. Temperature and observed rain still come from the nearest
  station `DASHBOARD_CWA_STATION` (township `溫度` is only a fallback).
- Rain-icon honesty: `cwa_condition_from_text(text, allow_precip)` only trusts a
  雷/雨/雪 phrase when it is actually raining or `rain_pct >= RAIN_CONDITION_PCT`
  (default 101, so observed rain only). Otherwise the phrase reads through to its dry sky state
  (多雲/晴/陰), so a low-probability forecast no longer shows a rain icon on a
  clear day.
- Output is written atomically to `DASHBOARD_OUTPUT`. Config via env / a `.env`
  next to the script. Deployed by `server/esp32-dashboard.{service,timer}`
  (timer runs every 1 min; device fetches every 60 s). The deployed collector
  file is root-owned — deploy with `scp` to `/tmp` then `sudo cp`.
