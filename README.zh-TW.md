# ESP32-S3 AI 用量與天氣儀表板

[English](README.md) | [繁體中文](README.zh-TW.md)

這是一個給 3.5 吋 ESP32-S3 N16R8 電容觸控螢幕使用的小型儀表板韌體。畫面會顯示：

- 天氣圖示、城市、溫度與降雨機率
- NTP 即時時鐘，顯示在天氣面板右上角
- Claude 5 小時與每週剩餘額度
- Codex 5 小時與每週剩餘額度

韌體本身是顯示端／thin client。若沒有設定 Wi-Fi 或 API URL，會以離線 demo mode 執行。

執行時行為：

- **無閃爍繪圖**：整個畫面先畫到 PSRAM sprite 雙緩衝，再一次推到螢幕。
- **輕量動畫**：agent 吉祥物會眨眼，天氣圖示會微動，由每秒一次的 render tick 驅動。
- **即時時鐘**：NTP 同步 `HH:MM`，時區為台北 `CST-8`。
- **Wi-Fi 自動重連**：路由器重開後會自動恢復，不會卡在 demo mode。
- **資料新鮮度提示**：天氣面板左下角在線時顯示 `updated Nm ago`，無法連 API 時顯示 `OFFLINE`。
- **夜間調光**：本地時間 23:00 到 07:00，背光會從 210 降到 70。

完整架構與修改指南，包括資料流、檔案職責、render pipeline 與常見任務，請看 [`AGENTS.md`](AGENTS.md)。

## 照片

<p>
  <img src="docs/images/20260701_123010.jpg" alt="Dashboard home view" width="240">
  <img src="docs/images/20260701_123016.jpg" alt="Weather detail view" width="240">
  <img src="docs/images/20260701_123034.jpg" alt="Settings view" width="240">
</p>

## 硬體目標

本專案目標是先前連結的淘寶開發板：

- ESP32-S3-WROOM-1-N16R8
- 16MB QSPI flash + 8MB PSRAM
- 3.5 吋 320x480 IPS LCD
- ST7796，8080 16-bit parallel bus
- GT1151Q 電容觸控控制器
- USB-C、CH340 serial、UART header、TF card slot

螢幕 pin map 參考 vendor package：`慧勤智远 ESP32-S3 N16R8 V1.0-3.5寸电容屏开发套件`。

## 架構

兩個部分透過一個 HTTP JSON 檔溝通。**server** 負責蒐集資料並發佈
`dashboard.json`；**firmware**（thin client）負責抓取並顯示。兩者解耦——server
每約 3 分鐘更新一次，裝置每 2 分鐘抓一次——所以誰都不會卡住誰。

```
 server/  (小型 VPS)                                firmware/  (ESP32-S3 裝置)
 ┌────────────────────────────┐                    ┌─────────────────────────────┐
 │ dashboard_collector.py      │   寫入             │ main.cpp                    │
 │  • Codex app-server + ccusage│ ─────────┐         │  • 每 120 秒 HTTP GET       │
 │  • CWA / open-meteo 天氣    │          ▼         │  • 解析 JSON                │
 │  • AQI / UV / 日出日落      │   dashboard.json   │  • 繪製到 320x480 LCD       │
 │ systemd 每 3 分鐘執行       │   (由 nginx  ──HTTP──▶ (PSRAM 雙緩衝)           │
 │ nginx 提供 /dashboard.json  │    提供)           │  • 觸控 UI、Wi-Fi 設定      │
 └────────────────────────────┘                    └─────────────────────────────┘
```

JSON 格式就是兩者之間的契約（見 [API Payload](#api-payload) 與
`server/dashboard.sample.json`）。

## 事前準備與 API 金鑰

**不需要任何付費 API。** 唯一可選的金鑰是台灣氣象服務用的；其餘服務要嘛免金鑰，
要嘛是你已經有的本機 CLI。

**伺服器端（collector）：**

| 項目 | 是否必要 | 取得方式 | 說明 |
|------|---------|----------|------|
| Python 3.10+ | 必要 | — | 執行 `dashboard_collector.py` |
| Node.js / `npx` | Claude 用量所需 | nodejs.org | 用來跑 [`ccusage`](https://www.npmjs.com/package/ccusage)，讀取本機 Claude Code 用量 |
| Codex CLI（已登入） | Codex 用量所需 | 你的 Codex 安裝 | collector 會呼叫 `codex app-server --stdio` 讀即時額度 |
| `CWA_API_KEY` | **可選** | [opendata.cwa.gov.tw](https://opendata.cwa.gov.tw/)（免費註冊） | 台灣鄉鎮級降雨預報。留空則自動退回 open-meteo |

collector 會自動呼叫、且免註冊的服務：

- **open-meteo**（`api.open-meteo.com`）——溫度、降雨、AQI、UV、日出日落。當 `CWA_API_KEY` 為空時的天氣來源，因此本專案在**世界任何地方**都能用，不限台灣。
- **OpenStreetMap Nominatim**——把座標反查成在地化（中文）城市名。
- **ip-api.com**——僅在 `DASHBOARD_AUTO_LOCATION=1` 且未設定固定座標時使用。

**裝置端（firmware）：** 只需你的 Wi-Fi SSID/密碼與 collector 的 JSON URL（見
[`firmware/src/dashboard_config.h`](#firmwaresrcdashboard_configh)），裝置上不存任何雲端金鑰。

### 最少要填的東西

1. **伺服器** — 把 `server/.env.example` 複製成 `.env`，至少設定：
   - `DASHBOARD_LAT` / `DASHBOARD_LON` — 你的座標（天氣準確度）。
   - `DASHBOARD_OUTPUT` — nginx 對外提供 `dashboard.json` 的路徑。
   - *（可選）* `CWA_API_KEY`，以及 `DASH_USD_TWD` 做費用換算。
2. **韌體** — 把 `firmware/src/dashboard_config.example.h` 複製成
   `dashboard_config.h`，設定 `WIFI_SSID`、`WIFI_PASSWORD`、`DASHBOARD_API_URL`
   （留空則進入 demo mode 先體驗）。

兩個真正的設定檔都被 git-ignore，所以你的 Wi-Fi 密碼與任何金鑰都不會進版控。

## 檔案

repo 結構——兩個並排資料夾，加上根目錄的文件：

```
firmware/   ESP32-S3 PlatformIO 專案（裝置端）
  platformio.ini            build/upload 環境（default + lcd-smoke-*）
  boards/                   本地 board manifest（16MB flash + PSRAM）
  src/board_config.h        LovyanGFX 螢幕／觸控 pin map
  src/main.cpp              所有應用邏輯：繪圖、天氣圖示、觸控 UI、
                            Wi-Fi 掃描／小鍵盤／fetch、JSON parsing
  src/dashboard_config.h    本機佈署設定（git-ignore；含真實憑證）
  src/dashboard_config.example.h   範本 -> 複製成 dashboard_config.h

server/     資料 collector + 佈署（VPS 端）
  dashboard_collector.py    產生 dashboard.json（用量 + 天氣）
  test_dashboard_collector.py   regression 測試
  mock_dashboard_server.py  本機測試韌體用的假 API
  dashboard.sample.json     標準範例 payload = API 契約
  .env.example              collector 設定範本（複製成 .env）
  esp32-dashboard.service/.timer   systemd 單元
  nginx-esp32-dashboard.conf       nginx 站台設定

docs/images/                README 使用的 dashboard 照片
```

## 設定

韌體會把 runtime settings 存在 ESP32 NVS Preferences。長按 dashboard 約 1.2 秒可進入機身上的 Settings 頁：

- 點 **Wi-Fi**
- 等待掃描清單
- 點選你的 SSID
- 用螢幕上的 QWERTY 小鍵盤輸入密碼
- `123` 切換符號，`^` 切換大寫，`<x` 刪除，`OK` 儲存並連線

預設 JSON URL：

```text
http://<your-server-host>/dashboard.json
```

沒有設定 Wi-Fi 時會維持 demo mode。機身上的 Wi-Fi 流程只負責設定 SSID/password；dashboard JSON URL 來自編譯期／本機設定 `firmware/src/dashboard_config.h`。

### `firmware/src/dashboard_config.h`

所有編譯期、與佈署相關的值都集中在這一個檔案（從 `firmware/src/dashboard_config.example.h`
複製；因為可能含真實憑證所以被 git ignore）。`firmware/src/main.cpp` 裡不再寫死任何使用者專屬的值。

必填（留空則維持 demo mode）：

- `WIFI_SSID`、`WIFI_PASSWORD` — 初始 Wi-Fi 憑證（也可在機身上修改）。
- `DASHBOARD_API_URL` — collector 的 JSON 端點。
- `DASHBOARD_CITY` — 備援城市名稱。
- `DASHBOARD_REFRESH_MS` — 抓取間隔（預設 120000）。

可選覆寫（在你的副本裡取消註解即可；範本內附預設值）：

- `DASHBOARD_CLOCK_TZ` — 機身時鐘的 POSIX 時區（預設 `CST-8`，即台北 UTC+8）。**不在台灣請務必修改**，例如 `JST-9`、`EST5EDT,M3.2.0,M11.1.0`。
- `DASHBOARD_NTP_1`、`DASHBOARD_NTP_2` — 對時用的 NTP 伺服器。
- `DASHBOARD_SETUP_AP_SSID`、`DASHBOARD_SETUP_AP_PASSWORD` — 首次開機的設定熱點（密碼需 >= 8 碼）。

介面語言（English／繁體中文）、主題、亮度是在機身 Settings 內即時選擇並存到 NVS，不屬於這個檔案。

建置真實 collector 前，可先用 mock server 測試 live update：

```bash
python3 server/mock_dashboard_server.py --host 0.0.0.0 --port 8080
```

`DASHBOARD_API_URL` 要設成電腦在 LAN 裡的 IP，不要用 `localhost`。例如：

```cpp
#define DASHBOARD_API_URL "http://192.168.1.20:8080/dashboard.json"
```

## API Payload

韌體預期的 JSON 形狀如下：

```json
{
  "weather": {
    "city": "Taipei",
    "temp_c": 27,
    "condition": "partly_cloudy",
    "label": "Partly cloudy",
    "rain_pct": 40,
    "rain_mm": 0.0,
    "is_raining": false,
    "rain_alert": false,
    "aqi": 42,
    "uv": 8,
    "sunrise": "05:08",
    "sunset": "18:48",
    "city_zh": "台北市",
    "weekday": "Tue",
    "day": "30",
    "month": "Jun",
    "year": "2026",
    "date": "Jun 30",
    "hourly": [
      { "t": "14", "temp": 34, "rain": 45 },
      { "t": "15", "temp": 33, "rain": 50 }
    ]
  },
  "claude": {
    "h5": { "used_pct": 28, "reset": "2h10m", "tokens": 1240000, "cost_twd": 18 },
    "weekly": { "used_pct": 59, "reset": "5d4h", "tokens": 18700000, "cost_twd": 260 },
    "daily": [0, 120000, 450000, 320000, 0, 800000, 1240000],
    "total_tok": 24200000,
    "total_twd": 412,
    "models": [ { "name": "opus-4-8", "pct": 78, "tok": 18800000 } ]
  },
  "codex": {
    "h5": { "used_pct": 18, "reset": "4h7m", "tokens": 980000, "cost_twd": 11 },
    "weekly": { "used_pct": 34, "reset": "6d1h", "tokens": 15600000, "cost_twd": 180 },
    "daily": [0, 90000, 210000, 500000, 0, 700000, 980000],
    "total_tok": 21100000,
    "total_twd": 305
  },
  "meta": {
    "source": "server dashboard",
    "quota_source": "live",
    "generated_at": "2026-06-30T14:00:00+08:00",
    "timezone": "Asia/Taipei"
  }
}
```

`used_pct` 代表已使用百分比，韌體會用它計算剩餘百分比。如果某個 quota window 查不到，請省略 `used_pct` 並送 `status: "unavailable"`；韌體會顯示 `--%`，不會假裝還有 100% 可用。如果能確定該 window 已用完，請送 `used_pct: 100`，韌體會顯示剩餘 `0%`。`tokens`、`cost_twd`、`daily`、`total_tok`、`total_twd`、`models`、`weather.hourly` 與 `meta` 都是 optional；存在時會提供 detail/weather 頁面的進階資訊。支援的天氣關鍵字包含 `sunny`、`partly_cloudy`、`cloudy`、`rain`、`thunderstorm`、`fog`、`night`、`night_cloudy`。

天氣可選欄位：`aqi`（美制 AQI，磚上依等級變色）、`uv`（最大紫外線指數）、`sunrise`/`sunset`（`HH:MM`）、`city_zh`（切換成繁體中文介面時顯示的在地地名）。`agent.models` 每筆（`name`、`pct`、`tok`）驅動 detail 頁的模型分佈長條。介面語言（English／中文）與主題在裝置的 Settings 內切換，JSON 本身維持語言中立。

## Build And Upload

韌體是位於 `firmware/` 的 PlatformIO 專案。需要 PlatformIO，並在 repo 根目錄用
`-d firmware` 執行（或先 `cd firmware`）：

```bash
pio run -d firmware
pio run -d firmware -t upload
pio device monitor -d firmware
```

這個 workspace 也有本地 `.venv` 版 PlatformIO，可以直接使用：

```bash
.venv/bin/pio run -d firmware
.venv/bin/pio run -d firmware -t upload
```

專案預設 upload port 是 `/dev/ttyACM0`，符合這台機器上連接的 ESP32-S3 USB JTAG/serial 裝置。

## Server Collector

server 目前提供：

```text
http://<your-server-host>/dashboard.json
```

server 上安裝的檔案（路徑僅為範例，請依你的主機調整）：

- `/opt/esp32-dashboard/dashboard_collector.py`
- `/opt/esp32-dashboard/.env`（由 `.env.example` 複製；git-ignore）
- `/etc/systemd/system/esp32-dashboard.service`
- `/etc/systemd/system/esp32-dashboard.timer`
- `/etc/nginx/sites-available/esp32-dashboard`
- `/var/www/esp32-dashboard/dashboard.json`

timer 每 3 分鐘執行一次。常用檢查：

```bash
systemctl status esp32-dashboard.timer --no-pager
systemctl status esp32-dashboard.service --no-pager
curl -fsS http://<your-server-host>/dashboard.json | python3 -m json.tool
```

### server `.env` 參考

把 `server/.env.example` 複製成 collector 旁邊的 `.env` 再填。主要變數：

| 變數 | 預設 | 用途 |
|------|------|------|
| `DASHBOARD_LAT` / `DASHBOARD_LON` | *(空)* | 天氣用的固定座標，強烈建議設定（優於 IP 猜測）。 |
| `DASHBOARD_CITY` | `Taipei` | 卡片上顯示的城市名。 |
| `DASHBOARD_OUTPUT` | `/var/www/esp32-dashboard/dashboard.json` | JSON 寫出的路徑（由 nginx 提供）。 |
| `CWA_API_KEY` | *(空)* | 可選的台灣 CWA 金鑰；留空則用 open-meteo。 |
| `DASHBOARD_CWA_LOCATION` / `_STATION` / `_TOWN` | `臺北市` / `臺北` / 空 | CWA 縣市／鄉鎮定位（需金鑰）。 |
| `DASHBOARD_AUTO_LOCATION` | `0` | `1` = 未設座標時，從裝置 IP 推城市。 |
| `DASHBOARD_RAIN_ALERT_PCT` | `90` | 琥珀色預報警示所需的降雨機率 %。 |
| `DASHBOARD_RAIN_CONDITION_PCT` | `101` | CWA 預報文字可改成雨圖示的機率；`101` 代表只有實測降雨才顯示。 |
| `DASHBOARD_RAIN_LOOKAHEAD_HOURS` | `0` | 可觸發警示的額外未來小時；`0` 代表只看目前小時。 |
| `DASHBOARD_RAIN_ALERT_PRECIP_MM` | `1.5` | 同一小時需達到的預估雨量，作為琥珀警示佐證。 |
| `DASHBOARD_WEATHER_CACHE_TTL` | `90` | 天氣快取秒數。 |
| `DASH_CLAUDE_5H_LIMIT` / `_WEEKLY_LIMIT` | `50000000` / `100000000` | 計算 Claude 已用 % 的 token 上限。 |
| `DASH_USD_TWD` | `32.5` | `cost_twd` 用的美元→台幣匯率。 |
| `DASH_CCUSAGE_NPX_PACKAGE` | `ccusage@20.0.14` | 由 `npx` 執行的 `ccusage` 版本。 |

Codex quota 會直接啟動目前的 Codex CLI：`codex app-server --stdio`，再用
JSON-RPC `account/rateLimits/read` 讀取。舊的 TUI/log 路徑只當 fallback，
所以 CLI 更新後即使背景 app-server 還停在舊版，也不會擋住新的 quota 查詢。

天氣位置準確度優先使用 server `.env` 的固定座標 `DASHBOARD_LAT` /
`DASHBOARD_LON`。如果兩者留空，也可以用 `DASHBOARD_AUTO_LOCATION=1` 從
nginx log 裡最近的 ESP32 requester IP 做城市級定位。預設只有目前小時能觸發
琥珀色預報警示；之後的小時仍會顯示在 hourly 頁面。警示必須在同一小時同時
達到機率與預估雨量門檻，且不會把主天氣圖示改成正在下雨；缺少預估雨量時也
不會只靠機率示警。

## Sources

- 淘寶商品頁：`https://world.taobao.com/lang/en-us/item/946264202563.htm`
- Vendor examples：`慧勤智远 ESP32-S3 N16R8 V1.0-3.5寸电容屏开发套件`
