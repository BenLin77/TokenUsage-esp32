#!/usr/bin/env python3
"""Build the ESP32 dashboard JSON from the server's local agent usage logs."""

from __future__ import annotations

import json
import math
import os
import re
import select
import shlex
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo


def load_dotenv(path: Path) -> None:
  """Minimal .env loader so the collector works under systemd and manual runs."""
  try:
    lines = path.read_text(encoding="utf-8").splitlines()
  except OSError:
    return
  for line in lines:
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
      continue
    key, _, value = line.partition("=")
    key = key.strip()
    value = value.strip().strip('"').strip("'")
    if key and key not in os.environ:
      os.environ[key] = value


load_dotenv(Path(__file__).resolve().parent / ".env")


TZ_NAME = os.environ.get("DASHBOARD_TZ", "Asia/Taipei")
TZ = ZoneInfo(TZ_NAME)
CITY = os.environ.get("DASHBOARD_CITY", "Taipei")
DEFAULT_LAT = "25.0330"
DEFAULT_LON = "121.5654"
LAT = os.environ.get("DASHBOARD_LAT", "").strip()
LON = os.environ.get("DASHBOARD_LON", "").strip()
OUTPUT_PATH = Path(os.environ.get("DASHBOARD_OUTPUT", "/var/www/esp32-dashboard/dashboard.json"))

CWA_API_KEY = os.environ.get("CWA_API_KEY", "").strip()
CWA_LOCATION = os.environ.get("DASHBOARD_CWA_LOCATION", "臺北市")
CWA_STATION = os.environ.get("DASHBOARD_CWA_STATION", "臺北")
# District-accurate forecast: when DASHBOARD_CWA_TOWN is set (e.g. "中和區"),
# read CWA's 鄉鎮預報 (township) dataset instead of the coarse county forecast
# (F-C0032-001). The county forecast only knows 22 縣市, so a whole-city phrase
# like "午後雷陣雨" paints rain on a district that stayed dry. The township
# dataset id is per-county (F-D0047-069 = 新北市). Leave DASHBOARD_CWA_TOWN empty
# to keep the county path. Temperature/observed rain still come from CWA_STATION.
CWA_TOWN = os.environ.get("DASHBOARD_CWA_TOWN", "").strip()
CWA_TOWNSHIP_DATASET = os.environ.get("DASHBOARD_CWA_TOWNSHIP_DATASET", "F-D0047-069")
# Amber "likely rain soon" heads-up threshold. Observed rain (rain_mm > 0)
# always alerts (red) regardless of this.
RAIN_ALERT_PCT = int(os.environ.get("DASHBOARD_RAIN_ALERT_PCT", "70"))
# CWA's Wx phrase is a city-wide, multi-hour forecast, so a summer "午後雷陣雨"
# wording paints a thunderstorm icon on a dry, sunny afternoon. Only let the
# rain/thunderstorm *icon* stand when it is actually raining now (rain_mm > 0)
# or the forecast probability is at least this high; otherwise fall back to the
# non-precip reading of the same phrase (多雲/晴/陰...).
RAIN_CONDITION_PCT = int(os.environ.get("DASHBOARD_RAIN_CONDITION_PCT", "60"))
# Only look at the current hour + this many hours ahead when promoting the card
# to an imminent-rain alert. A wide window let a lone spike 2-3 h out flip the
# card to red on afternoons that stayed dry, so keep this tight.
RAIN_LOOKAHEAD_HOURS = int(os.environ.get("DASHBOARD_RAIN_LOOKAHEAD_HOURS", "1"))
# A high forecast probability alone over-triggers on dry convective afternoons.
# Require the model to also expect at least this much rain (mm) in the imminent
# window before flipping the card to "Rain soon". Set to 0 to disable.
RAIN_ALERT_PRECIP_MM = float(os.environ.get("DASHBOARD_RAIN_ALERT_PRECIP_MM", "0.2"))
AUTO_LOCATION = os.environ.get("DASHBOARD_AUTO_LOCATION", "0").strip().lower() in {"1", "true", "yes", "on"}
ESP32_ACCESS_LOG = Path(os.environ.get("DASHBOARD_NGINX_ACCESS_LOG", "/var/log/nginx/esp32-dashboard.access.log"))
GEOLOCATION_URL = os.environ.get(
    "DASHBOARD_GEOLOCATION_URL",
    "http://ip-api.com/json/{ip}?fields=status,message,city,lat,lon,timezone,query",
)

WEATHER_CACHE_PATH = Path(
    os.environ.get("DASHBOARD_WEATHER_CACHE", str(Path(__file__).resolve().parent / ".weather_cache.json"))
)
WEATHER_CACHE_TTL = int(os.environ.get("DASHBOARD_WEATHER_CACHE_TTL", "90"))
LOCATION_CACHE_PATH = Path(
    os.environ.get("DASHBOARD_LOCATION_CACHE", str(Path(__file__).resolve().parent / ".location_cache.json"))
)
LOCATION_CACHE_TTL = int(os.environ.get("DASHBOARD_LOCATION_CACHE_TTL", "21600"))
CITY_ZH_CACHE_PATH = Path(
    os.environ.get("DASHBOARD_CITY_ZH_CACHE", str(Path(__file__).resolve().parent / ".city_zh_cache.json"))
)

CLAUDE_5H_LIMIT = int(os.environ.get("DASH_CLAUDE_5H_LIMIT", "50000000"))
CLAUDE_WEEKLY_LIMIT = int(os.environ.get("DASH_CLAUDE_WEEKLY_LIMIT", "100000000"))

# Codex records authoritative rate limits (real used % + reset epoch) into this
# trace DB on every startup "websocket warmup". Launching the Codex TUI (which
# the collector already does) refreshes it when Codex can start. If startup is
# blocked by a limit/auth issue, keep any still-active windows from the log and
# mark missing windows unavailable instead of deriving fake quota from ccusage.
CODEX_LOGS_DB = Path(
    os.environ.get("DASH_CODEX_LOGS_DB", str(Path.home() / ".codex" / "logs_2.sqlite"))
)

# USD->TWD rate for the API-equivalent cost shown on the detail pages.
USD_TWD = float(os.environ.get("DASH_USD_TWD", "32.5"))

# Days of daily token history to send for the detail-page bar chart.
DAILY_CHART_DAYS = int(os.environ.get("DASH_CHART_DAYS", "14"))

# Keep production collection reproducible. Override DASH_CCUSAGE_CMD to use a
# preinstalled binary, or DASH_CCUSAGE_NPX_PACKAGE to intentionally update.
CCUSAGE_NPX_PACKAGE = os.environ.get("DASH_CCUSAGE_NPX_PACKAGE", "ccusage@20.0.14")
CCUSAGE_CMD = shlex.split(os.environ.get("DASH_CCUSAGE_CMD", "")) or ["npx", "-y", CCUSAGE_NPX_PACKAGE]
CODEX_APP_SERVER_CMD = shlex.split(os.environ.get("DASH_CODEX_APP_SERVER_CMD", "")) or [
    "codex",
    "app-server",
    "--stdio",
]


def run_json(args: list[str], timeout: int = 120) -> dict:
  result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
  if result.returncode != 0:
    raise RuntimeError(f"{' '.join(args)} failed: {result.stderr.strip()}")
  return json.loads(result.stdout)


def ccusage(*args: str) -> dict:
  return run_json([*CCUSAGE_CMD, *args])


def parse_instant(value: str) -> datetime:
  return datetime.fromisoformat(value.replace("Z", "+00:00"))


def pct(tokens: int, limit: int) -> int:
  if limit <= 0:
    return 0
  return max(0, min(100, round((tokens * 100) / limit)))


def quota_usage(used_pct: int, reset: str) -> dict:
  return {"used_pct": max(0, min(100, int(used_pct))), "reset": reset}


def unavailable_quota(reset: str = "--", status: str = "unavailable") -> dict:
  return {"reset": reset, "status": status}


def unknown_usage() -> dict:
  return {
      "h5": unavailable_quota(),
      "weekly": unavailable_quota(),
  }


def quota_known(block: object) -> bool:
  return isinstance(block, dict) and isinstance(block.get("used_pct"), int)


def usage_complete(usage: dict) -> bool:
  return all(quota_known(usage.get(window)) for window in ("h5", "weekly"))


def next_monday_label() -> str:
  return "Mon"


def time_label(value: str | None) -> str:
  if not value:
    return "--"
  return parse_instant(value).astimezone(TZ).strftime("%H:%M")


WEEKDAY_INDEX = {"mon": 0, "tue": 1, "wed": 2, "thu": 3, "fri": 4, "sat": 5, "sun": 6}
MONTH_INDEX = {
    "jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
    "jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
}


def humanize_delta(delta: timedelta) -> str:
  """Compact 'time remaining' string, e.g. '2h10m', '3d4h', '45m'."""
  secs = int(delta.total_seconds())
  if secs <= 0:
    return "soon"
  days, rem = divmod(secs, 86400)
  hours, rem = divmod(rem, 3600)
  mins = rem // 60
  if days:
    return f"{days}d{hours}h"
  if hours:
    return f"{hours}h{mins}m"
  return f"{mins}m"


def next_weekday_midnight(now: datetime, target_idx: int) -> datetime:
  days_ahead = (target_idx - now.weekday()) % 7
  if days_ahead == 0:
    days_ahead = 7  # the upcoming reset, never "today"
  return (now + timedelta(days=days_ahead)).replace(hour=0, minute=0, second=0, microsecond=0)


def parse_clock_target(text: str, now: datetime) -> datetime | None:
  match = re.match(r"^\s*(\d{1,2})(?::(\d{2}))?\s*([ap]m)?\s*$", text, re.IGNORECASE)
  if not match:
    return None
  hour = int(match.group(1))
  minute = int(match.group(2) or 0)
  ampm = (match.group(3) or "").lower()
  if ampm == "pm" and hour < 12:
    hour += 12
  if ampm == "am" and hour == 12:
    hour = 0
  if hour > 23 or minute > 59:
    return None
  target = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
  if target <= now:
    target += timedelta(days=1)
  return target


def parse_date_target(text: str, now: datetime) -> datetime | None:
  """Parse 'Jul 3' / 'Jul 3 2026' style dates (as shown by the Claude TUI)."""
  match = re.match(r"^\s*([A-Za-z]{3,9})\.?\s+(\d{1,2})(?:\s+(\d{4}))?\s*$", text)
  if not match:
    return None
  month = MONTH_INDEX.get(match.group(1)[:3].lower())
  if not month:
    return None
  day = int(match.group(2))
  year = int(match.group(3)) if match.group(3) else now.year
  try:
    target = now.replace(year=year, month=month, day=day, hour=0, minute=0, second=0, microsecond=0)
  except ValueError:
    return None
  # A bare date is anchored at midnight, so "today's" reset would look like it
  # already passed. Only roll a yearless date forward when it is strictly before
  # today (avoids reporting a same-day weekly reset as ~364 days out).
  if match.group(3) is None and target.date() < now.date():
    target = target.replace(year=year + 1)
  return target


def parse_datetime_target(text: str, now: datetime) -> datetime | None:
  """Parse 'Jul 3, 4:59pm' — a date plus a clock time, as the Claude TUI shows
  for the weekly reset. Keeping the time avoids collapsing a same-day reset to
  midnight (which would render as "soon" for the whole day)."""
  if "," not in text:
    return None
  date_part, time_part = (chunk.strip() for chunk in text.split(",", 1))
  date_target = parse_date_target(date_part, now)
  if date_target is None:
    return None
  match = re.match(r"^\s*(\d{1,2})(?::(\d{2}))?\s*([ap]m)?\s*$", time_part, re.IGNORECASE)
  if not match:
    return None
  hour = int(match.group(1))
  minute = int(match.group(2) or 0)
  ampm = (match.group(3) or "").lower()
  if ampm == "pm" and hour < 12:
    hour += 12
  if ampm == "am" and hour == 12:
    hour = 0
  if hour > 23 or minute > 59:
    return None
  return date_target.replace(hour=hour, minute=minute, second=0, microsecond=0)


def reset_countdown(reset: str, now: datetime) -> str:
  """Turn a reset moment ('14:30', '2:30am', 'Mon', 'Jul 3', 'Jul 3, 4:59pm')
  into time-remaining.

  Rolling/unknown markers ('roll', '5h', '--') are left untouched.
  """
  if not reset:
    return reset
  text = reset.strip()
  key = text[:3].lower()
  if key in WEEKDAY_INDEX:
    return humanize_delta(next_weekday_midnight(now, WEEKDAY_INDEX[key]) - now)
  target = (
      parse_datetime_target(text, now)
      or parse_clock_target(text, now)
      or parse_date_target(text, now)
  )
  if target is not None:
    return humanize_delta(target - now)
  return reset


def safe_float(value: object) -> float | None:
  try:
    return float(value)
  except (TypeError, ValueError):
    return None


def valid_coordinates(lat: object, lon: object) -> bool:
  lat_f = safe_float(lat)
  lon_f = safe_float(lon)
  return lat_f is not None and lon_f is not None and -90 <= lat_f <= 90 and -180 <= lon_f <= 180


def location_payload(city: str, lat: object, lon: object, source: str) -> dict:
  return {
      "city": city or CITY,
      "lat": str(lat),
      "lon": str(lon),
      "source": source,
  }


def read_location_cache(now: datetime) -> dict | None:
  try:
    cached = json.loads(LOCATION_CACHE_PATH.read_text(encoding="utf-8"))
    fetched_at = datetime.fromisoformat(cached["fetched_at"])
  except (OSError, ValueError, KeyError, json.JSONDecodeError):
    return None
  if (now - fetched_at).total_seconds() > LOCATION_CACHE_TTL:
    return None
  location = cached.get("location")
  if not isinstance(location, dict) or not valid_coordinates(location.get("lat"), location.get("lon")):
    return None
  return location


def write_location_cache(now: datetime, location: dict) -> None:
  payload = {"fetched_at": now.isoformat(), "location": location}
  try:
    LOCATION_CACHE_PATH.write_text(json.dumps(payload), encoding="utf-8")
  except OSError:
    pass


def latest_esp32_client_ip() -> str | None:
  try:
    with ESP32_ACCESS_LOG.open("rb") as handle:
      handle.seek(0, os.SEEK_END)
      size = handle.tell()
      handle.seek(max(0, size - 262144), os.SEEK_SET)
      lines = handle.read().decode("utf-8", errors="ignore").splitlines()
  except OSError:
    return None

  for line in reversed(lines):
    if "ESP32HTTPClient" not in line or "GET /dashboard.json" not in line:
      continue
    match = re.match(r"^(\S+)\s+", line)
    if match:
      return match.group(1)
  return None


def ip_geolocation(ip: str) -> dict | None:
  if not ip:
    return None
  url = GEOLOCATION_URL.format(ip=urllib.parse.quote(ip, safe=""))
  try:
    with urllib.request.urlopen(url, timeout=8) as response:
      data = json.loads(response.read().decode("utf-8"))
  except Exception:
    return None
  if data.get("status") not in (None, "success"):
    return None
  if not valid_coordinates(data.get("lat"), data.get("lon")):
    return None
  return location_payload(str(data.get("city") or CITY), data["lat"], data["lon"], "auto_ip")


def weather_location(now: datetime) -> dict:
  if valid_coordinates(LAT, LON):
    return location_payload(CITY, LAT, LON, "fixed")

  if AUTO_LOCATION:
    cached = read_location_cache(now)
    if cached:
      return cached
    ip = latest_esp32_client_ip()
    location = ip_geolocation(ip) if ip else None
    if location:
      write_location_cache(now, location)
      return location

  return location_payload(CITY, DEFAULT_LAT, DEFAULT_LON, "default")


def cwa_fetch(dataset: str, params: dict) -> dict:
  query = {"Authorization": CWA_API_KEY, "format": "JSON", **params}
  url = (
      "https://opendata.cwa.gov.tw/api/v1/rest/datastore/"
      + dataset + "?" + urllib.parse.urlencode(query)
  )
  with urllib.request.urlopen(url, timeout=12) as response:
    return json.loads(response.read().decode("utf-8"))


def cwa_condition_from_text(text: str, allow_precip: bool = True) -> tuple[str, str]:
  """Map a CWA Chinese weather phrase (Wx) to dashboard condition/label.

  With ``allow_precip`` false, the 雷/雨/雪 branches are skipped so a forecast
  phrase like "午後雷陣雨" reads through to its non-precip sky state (多雲/晴/陰),
  keeping the icon honest on dry afternoons.
  """
  if allow_precip:
    if "雷" in text:
      return "thunderstorm", "Thunderstorm"
    if "雨" in text:
      return "rain", "Rain"
    if "雪" in text:
      return "rain", "Snow"
  if "霧" in text or "靄" in text:
    return "fog", "Fog"
  if "晴" in text and "雲" not in text and "陰" not in text:
    return "sunny", "Sunny"
  if "晴" in text and "雲" in text:
    return "partly_cloudy", "Partly cloudy"
  if "陰" in text:
    return "cloudy", "Cloudy"
  if "多雲" in text:
    return "partly_cloudy", "Partly cloudy"
  return "cloudy", "Cloudy"


def cwa_observation() -> tuple[int | None, float]:
  """Current temperature and observed precipitation from the nearest station."""
  data = cwa_fetch("O-A0003-001", {"StationName": CWA_STATION})
  stations = data.get("records", {}).get("Station", [])
  for station in stations:
    element = station.get("WeatherElement", {})
    temp = safe_float(element.get("AirTemperature"))
    rain_mm = safe_float((element.get("Now") or {}).get("Precipitation"))

    temp_c = round(temp) if temp is not None and temp > -90 else None
    if rain_mm is None or rain_mm < 0:
      rain_mm = 0.0
    return temp_c, rain_mm
  return None, 0.0


def cwa_active_parameter(element: dict, now: datetime) -> dict:
  """Pick the forecast slot covering `now` (falls back to the first slot)."""
  slots = element.get("time", [])
  for slot in slots:
    try:
      start = datetime.strptime(slot["startTime"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ)
      end = datetime.strptime(slot["endTime"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ)
    except (KeyError, ValueError):
      continue
    if start <= now < end:
      return slot.get("parameter", {})
  return slots[0].get("parameter", {}) if slots else {}


def cwa_town_slot(element: dict, now: datetime) -> dict:
  """Pick the township ElementValue dict for the slot covering `now`.

  Township elements are either ranged (StartTime/EndTime — 天氣現象,
  3小時降雨機率) or instantaneous (DataTime — 溫度). Falls back to the first slot.
  """
  slots = element.get("Time", [])
  chosen = None
  for slot in slots:
    values = slot.get("ElementValue") or []
    value = values[0] if values else {}
    start_raw = slot.get("StartTime") or slot.get("DataTime")
    if not start_raw:
      continue
    try:
      start = datetime.fromisoformat(start_raw)
    except ValueError:
      continue
    end_raw = slot.get("EndTime")
    if end_raw:  # ranged slot: an exact cover wins outright
      try:
        end = datetime.fromisoformat(end_raw)
      except ValueError:
        continue
      if start <= now < end:
        return value
    elif start <= now:  # instantaneous: keep the most recent past reading
      chosen = value
  if chosen is not None:
    return chosen
  first = slots[0].get("ElementValue") if slots else None
  return first[0] if first else {}


def cwa_township_forecast(now: datetime) -> tuple[str, int, int | None]:
  """(weather phrase, rain %, temp) from the 鄉鎮 forecast for CWA_TOWN."""
  data = cwa_fetch(CWA_TOWNSHIP_DATASET, {
      "LocationName": CWA_TOWN,
      "ElementName": "天氣現象,3小時降雨機率,溫度",
  })
  group = data["records"]["Locations"][0]
  town = next(
      (loc for loc in group["Location"] if loc.get("LocationName") == CWA_TOWN),
      group["Location"][0],
  )
  elements = {e["ElementName"]: e for e in town["WeatherElement"]}

  wx_text = cwa_town_slot(elements.get("天氣現象", {}), now).get("Weather", "")

  pop = cwa_town_slot(elements.get("3小時降雨機率", {}), now)
  try:
    rain_pct = max(0, min(100, int(pop.get("ProbabilityOfPrecipitation", "0"))))
  except (TypeError, ValueError):
    rain_pct = 0

  temp = safe_float(cwa_town_slot(elements.get("溫度", {}), now).get("Temperature"))
  temp_c = round(temp) if temp is not None else None
  return wx_text, rain_pct, temp_c


def cwa_weather(now: datetime, location: dict | None = None) -> dict:
  """Build weather from a CWA forecast and current station precipitation.

  Uses the district-accurate 鄉鎮 (township) forecast when DASHBOARD_CWA_TOWN is
  set, otherwise the coarse 縣市 (county) forecast F-C0032-001. Temperature and
  observed rain always come from the nearest station (CWA_STATION).
  """
  fallback_temp: int | None = None
  if CWA_TOWN:
    wx_text, rain_pct, fallback_temp = cwa_township_forecast(now)
  else:
    forecast = cwa_fetch("F-C0032-001", {"locationName": CWA_LOCATION})
    forecast_location = forecast["records"]["location"][0]
    elements = {e["elementName"]: cwa_active_parameter(e, now) for e in forecast_location["weatherElement"]}
    wx_text = elements.get("Wx", {}).get("parameterName", "")
    try:
      rain_pct = max(0, min(100, int(elements.get("PoP", {}).get("parameterName", "0"))))
    except (TypeError, ValueError):
      rain_pct = 0
    min_t = safe_float(elements.get("MinT", {}).get("parameterName"))
    max_t = safe_float(elements.get("MaxT", {}).get("parameterName"))
    if min_t is not None and max_t is not None:
      fallback_temp = round((min_t + max_t) / 2)

  try:
    temp_c, rain_mm = cwa_observation()
  except Exception:
    temp_c, rain_mm = None, 0.0
  if temp_c is None:
    temp_c = fallback_temp if fallback_temp is not None else 27

  is_raining = rain_mm > 0
  # Trust a rain/thunderstorm phrase only when it is actually raining or the
  # forecast probability is high; otherwise show the phrase's dry sky state.
  allow_precip = is_raining or rain_pct >= RAIN_CONDITION_PCT
  condition, label = cwa_condition_from_text(wx_text, allow_precip)
  rain_alert = is_raining or rain_pct >= RAIN_ALERT_PCT

  return {
      "city": (location or {}).get("city") or CITY,
      "temp_c": temp_c,
      "condition": condition,
      "label": label,
      "rain_pct": rain_pct,
      "rain_mm": round(rain_mm, 1),
      "is_raining": is_raining,
      "rain_alert": rain_alert,
  }


def open_meteo_weather(location: dict | None = None) -> dict:
  loc = location or location_payload(CITY, LAT or DEFAULT_LAT, LON or DEFAULT_LON, "default")
  params = {
      "latitude": loc["lat"],
      "longitude": loc["lon"],
      "current": "temperature_2m,weather_code,precipitation,rain,showers,snowfall",
      "hourly": "precipitation_probability,precipitation",
      "forecast_days": "1",
      "timezone": TZ_NAME,
  }
  url = "https://api.open-meteo.com/v1/forecast?" + urllib.parse.urlencode(params)
  with urllib.request.urlopen(url, timeout=12) as response:
    data = json.loads(response.read().decode("utf-8"))

  current = data.get("current", {})
  weather_code = int(current.get("weather_code", 1))
  condition, label = condition_for_weather_code(weather_code)
  rain_pct = rain_probability(data, current.get("time"))
  rain_mm = max(
      safe_float(current.get("precipitation")) or 0.0,
      safe_float(current.get("rain")) or 0.0,
      safe_float(current.get("showers")) or 0.0,
  )
  is_raining = rain_mm > 0

  return {
      "city": loc["city"],
      "temp_c": round(float(current.get("temperature_2m", 27))),
      "condition": condition,
      "label": label,
      "rain_pct": rain_pct,
      "rain_mm": round(rain_mm, 1),
      "is_raining": is_raining,
      "rain_alert": is_raining or rain_pct >= RAIN_ALERT_PCT,
  }


def localized_city_name(location: dict | None) -> str:
  """Traditional-Chinese locality name (e.g. 中和區) via OSM Nominatim reverse
  geocoding. Cached by rounded coordinates so Nominatim only sees a request
  when the device actually moves; empty string when unavailable."""
  loc = location or {}
  lat = safe_float(loc.get("lat"))
  lon = safe_float(loc.get("lon"))
  if lat is None or lon is None:
    return ""
  key = f"{lat:.3f},{lon:.3f}"
  try:
    cache = json.loads(CITY_ZH_CACHE_PATH.read_text(encoding="utf-8"))
  except (OSError, ValueError):
    cache = {}
  if isinstance(cache, dict) and key in cache:
    return str(cache[key])

  params = {"lat": lat, "lon": lon, "format": "jsonv2",
            "accept-language": "zh-TW", "zoom": "13"}
  url = "https://nominatim.openstreetmap.org/reverse?" + urllib.parse.urlencode(params)
  request = urllib.request.Request(url, headers={"User-Agent": "esp32-dashboard/1.0"})
  try:
    with urllib.request.urlopen(request, timeout=12) as response:
      address = json.loads(response.read().decode("utf-8")).get("address", {})
  except Exception as error:
    print(f"city zh unavailable: {error}", file=sys.stderr)
    return ""

  name = ""
  # District-level first (matches the granularity of the English IP city).
  for field in ("suburb", "town", "city_district", "village", "city", "county", "state"):
    value = address.get(field)
    if value:
      name = str(value)
      break
  if name:
    cache = cache if isinstance(cache, dict) else {}
    cache[key] = name
    try:
      CITY_ZH_CACHE_PATH.write_text(json.dumps(cache, ensure_ascii=False), encoding="utf-8")
    except OSError:
      pass
  return name


def sun_time(lat: float, lon: float, day: datetime, tz_hours: float, is_rise: bool) -> str | None:
  """Local sunrise/sunset "HH:MM" from the almanac sunrise equation (no network).

  Returns None at latitudes where the sun does not rise/set that day.
  """
  n = day.timetuple().tm_yday
  lng_hour = lon / 15.0
  t = n + ((6 if is_rise else 18) - lng_hour) / 24.0
  m = 0.9856 * t - 3.289
  L = (m + 1.916 * math.sin(math.radians(m)) + 0.020 * math.sin(math.radians(2 * m)) + 282.634) % 360
  ra = math.degrees(math.atan(0.91764 * math.tan(math.radians(L)))) % 360
  ra += (math.floor(L / 90) * 90) - (math.floor(ra / 90) * 90)
  ra /= 15.0
  sin_dec = 0.39782 * math.sin(math.radians(L))
  cos_dec = math.cos(math.asin(sin_dec))
  cos_h = (math.cos(math.radians(90.833)) - sin_dec * math.sin(math.radians(lat))) / (
      cos_dec * math.cos(math.radians(lat)))
  if not -1 <= cos_h <= 1:
    return None
  h = (360 - math.degrees(math.acos(cos_h))) if is_rise else math.degrees(math.acos(cos_h))
  h /= 15.0
  local = (h + ra - 0.06571 * t - 6.622 - lng_hour + tz_hours) % 24
  hh = int(local)
  mm = int(round((local - hh) * 60))
  if mm == 60:
    hh = (hh + 1) % 24
    mm = 0
  return f"{hh:02d}:{mm:02d}"


def sun_and_air(location: dict | None) -> dict:
  """Sunrise/sunset (computed locally) + current UV and US AQI from open-meteo.

  Sunrise/sunset are pure math so they never depend on a reachable host. UV and
  AQI share one call to the air-quality API; the server cannot reach the main
  api.open-meteo.com forecast host, so UV is read from us_aqi's endpoint (which
  also exposes uv_index) rather than the daily forecast. Best effort: a failure
  just omits its keys so the device hides that line instead of showing fakes.
  """
  loc = location or location_payload(CITY, LAT or DEFAULT_LAT, LON or DEFAULT_LON, "default")
  out = {}
  lat = safe_float(loc.get("lat"))
  lon = safe_float(loc.get("lon"))
  if lat is not None and lon is not None:
    now = datetime.now(TZ)
    tz_hours = (now.utcoffset() or timedelta()).total_seconds() / 3600.0
    sunrise = sun_time(lat, lon, now, tz_hours, True)
    sunset = sun_time(lat, lon, now, tz_hours, False)
    if sunrise:
      out["sunrise"] = sunrise
    if sunset:
      out["sunset"] = sunset
  try:
    params = {"latitude": loc["lat"], "longitude": loc["lon"], "current": "us_aqi,uv_index"}
    url = "https://air-quality-api.open-meteo.com/v1/air-quality?" + urllib.parse.urlencode(params)
    with urllib.request.urlopen(url, timeout=12) as response:
      current = json.loads(response.read().decode("utf-8")).get("current", {})
    aqi = safe_float(current.get("us_aqi"))
    if aqi is not None:
      out["aqi"] = int(aqi)
    uv = safe_float(current.get("uv_index"))
    if uv is not None:
      out["uv"] = round(uv)
  except Exception as error:
    print(f"aqi/uv unavailable: {error}", file=sys.stderr)
  return out


def condition_for_weather_code(code: int) -> tuple[str, str]:
  if code == 0:
    return "sunny", "Sunny"
  if code in {1, 2}:
    return "partly_cloudy", "Partly cloudy"
  if code == 3:
    return "cloudy", "Cloudy"
  if code in {45, 48}:
    return "fog", "Fog"
  if 95 <= code <= 99:
    return "thunderstorm", "Thunderstorm"
  if 51 <= code <= 67 or 80 <= code <= 82:
    return "rain", "Rain"
  return "cloudy", "Cloudy"


def rain_probability(data: dict, current_time: str | None) -> int:
  hourly = data.get("hourly", {})
  times = hourly.get("time") or []
  probs = hourly.get("precipitation_probability") or []
  if not current_time or not times or not probs:
    return 0

  current_hour = current_time[:13]
  for index, value in enumerate(times):
    if str(value).startswith(current_hour) and index < len(probs):
      try:
        return max(0, min(100, int(probs[index] or 0)))
      except (TypeError, ValueError):
        return 0
  return 0


def read_weather_cache(now: datetime) -> dict | None:
  """Return cached weather if it is still within the TTL, else None."""
  try:
    cached = json.loads(WEATHER_CACHE_PATH.read_text(encoding="utf-8"))
    fetched_at = datetime.fromisoformat(cached["fetched_at"])
  except (OSError, ValueError, KeyError, json.JSONDecodeError):
    return None
  if (now - fetched_at).total_seconds() > WEATHER_CACHE_TTL:
    return None
  weather = cached.get("weather")
  return weather if isinstance(weather, dict) else None


def write_weather_cache(now: datetime, weather: dict) -> None:
  payload = {"fetched_at": now.isoformat(), "weather": weather}
  try:
    WEATHER_CACHE_PATH.write_text(json.dumps(payload), encoding="utf-8")
  except OSError:
    pass


def normalize_weather(weather: dict) -> dict:
  rain_mm = safe_float(weather.get("rain_mm"))
  if rain_mm is None or rain_mm < 0:
    rain_mm = 0.0
  rain_pct = int(safe_float(weather.get("rain_pct")) or 0)
  weather["rain_mm"] = round(rain_mm, 1)
  weather["is_raining"] = bool(weather.get("is_raining") or rain_mm > 0)
  # Authoritative rain-alert rule: observed rain (red) or a high forecast
  # probability (amber). A rain/thunderstorm *condition* alone does NOT alert —
  # it caused false alarms on dry forecasts. Firmware picks red vs amber from
  # is_raining.
  weather["rain_alert"] = bool(weather["is_raining"] or rain_pct >= RAIN_ALERT_PCT)
  return weather


def fallback_weather() -> dict:
  return {
      "city": CITY,
      "temp_c": 27,
      "condition": "partly_cloudy",
      "label": "Partly cloudy",
      "rain_pct": 40,
      "rain_mm": 0.0,
      "is_raining": False,
      "rain_alert": False,
  }


def compact_tui_reset(value: str) -> str:
  # Strip the trailing timezone note, e.g. "Jul 3, 4:59pm (Asia/Taipei)".
  value = re.sub(r"\s*\(.*?\)\s*$", "", value.strip())
  # The weekly line is "Mon D, H:MMam" — keep the time. Only drop the part after
  # the comma when it is a bare year ("Mon D, YYYY").
  if "," in value:
    head, tail = (chunk.strip() for chunk in value.split(",", 1))
    if re.fullmatch(r"\d{4}", tail):
      return head
  return value or "--"


def claude_usage(now: datetime, week_start: datetime) -> dict:
  tui_usage = claude_usage_from_tui()
  if tui_usage:
    return tui_usage

  h5 = unavailable_quota()
  active = ccusage("claude", "blocks", "--active", "--json", "--timezone", TZ_NAME, "--offline")
  for block in active.get("blocks", []):
    if not block.get("isGap"):
      h5_tokens = int(block.get("totalTokens") or 0)
      h5 = quota_usage(pct(h5_tokens, CLAUDE_5H_LIMIT), time_label(block.get("endTime")))
      break

  weekly_usage = unavailable_quota()
  weekly = ccusage(
      "claude",
      "weekly",
      "--json",
      "--timezone",
      TZ_NAME,
      "--offline",
      "-w",
      "monday",
      "--since",
      week_start.strftime("%Y%m%d"),
  )
  week_key = week_start.date().isoformat()
  for item in weekly.get("weekly", []):
    if item.get("week") == week_key:
      week_tokens = int(item.get("totalTokens") or 0)
      weekly_usage = quota_usage(pct(week_tokens, CLAUDE_WEEKLY_LIMIT), next_monday_label())
      break

  return {
      "h5": h5,
      "weekly": weekly_usage,
  }


def claude_usage_from_tui() -> dict | None:
  session = f"esp32_claude_usage_{os.getpid()}_{int(time.time())}"
  workdir = Path.home() / "code" / "esp32-dashboard" / "claude-usage-workdir"
  captures: list[str] = []
  try:
    workdir.mkdir(parents=True, exist_ok=True)
    subprocess.run(["tmux", "kill-session", "-t", session], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    subprocess.run(
        [
            "tmux",
            "new-session",
            "-d",
            "-s",
            session,
            "-x",
            "140",
            "-y",
            "42",
            f"cd {shlex.quote(str(workdir))} && TERM=xterm-256color claude --ax-screen-reader",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=5,
    )
    time.sleep(float(os.environ.get("DASH_CLAUDE_TUI_START_WAIT", "4")))
    subprocess.run(["tmux", "send-keys", "-t", session, "/status", "Enter"], check=True, timeout=5)
    time.sleep(float(os.environ.get("DASH_CLAUDE_TUI_STATUS_WAIT", "2")))

    for keys in (
        ["Tab", "Tab", "Tab", "Enter"],
        ["Right", "Right", "Right", "Enter"],
        ["Tab", "Tab", "Enter"],
    ):
      subprocess.run(["tmux", "send-keys", "-t", session, *keys], check=True, timeout=5)
      time.sleep(float(os.environ.get("DASH_CLAUDE_TUI_TAB_WAIT", "2")))
      captures.append(
          subprocess.run(
              ["tmux", "capture-pane", "-t", session, "-p", "-S", "-240"],
              check=True,
              capture_output=True,
              text=True,
              timeout=5,
          ).stdout
      )
  except Exception:
    return None
  finally:
    subprocess.run(["tmux", "kill-session", "-t", session], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)

  capture = "\n".join(captures)
  session_match = re.search(
      r"Current session\s+(\d+)%\s+\d+%\s+used\s+Resets\s+([^\n(]+)",
      capture,
      re.IGNORECASE,
  )
  week_match = re.search(
      r"Current week[^\n]*\s+(\d+)%\s+\d+%\s+used\s+Resets\s+([^\n(]+)",
      capture,
      re.IGNORECASE,
  )
  if not session_match or not week_match:
    return None

  return {
      "h5": {
          "used_pct": max(0, min(100, int(session_match.group(1)))),
          "reset": compact_tui_reset(session_match.group(2)),
      },
      "weekly": {
          "used_pct": max(0, min(100, int(week_match.group(1)))),
          "reset": compact_tui_reset(week_match.group(2)),
      },
  }


def codex_app_server_rpc(method: str, timeout: float = 15.0) -> dict:
  """Call one Codex app-server JSON-RPC method through a short-lived stdio server."""
  request_id = f"esp32-{method.replace('/', '-')}-{os.getpid()}-{int(time.time() * 1000)}"
  messages = [
      {
          "id": "initialize",
          "method": "initialize",
          "params": {
              "clientInfo": {"name": "esp32-dashboard", "version": "1.0"},
              "capabilities": {},
          },
      },
      {"method": "initialized"},
      {"id": request_id, "method": method},
  ]

  proc = subprocess.Popen(
      CODEX_APP_SERVER_CMD,
      stdin=subprocess.PIPE,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      text=True,
      encoding="utf-8",
  )
  stderr_lines: list[str] = []
  try:
    if proc.stdin is None or proc.stdout is None or proc.stderr is None:
      raise RuntimeError("codex app-server stdio pipes are unavailable")
    for message in messages:
      proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    proc.stdin.flush()

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
      remaining = max(0.0, deadline - time.monotonic())
      readable, _, _ = select.select([proc.stdout, proc.stderr], [], [], min(0.25, remaining))
      if not readable and proc.poll() is not None:
        break
      for stream in readable:
        line = stream.readline()
        if not line:
          continue
        if stream is proc.stderr:
          stderr_lines.append(line.strip())
          continue
        try:
          message = json.loads(line)
        except json.JSONDecodeError:
          continue
        if message.get("id") != request_id:
          continue
        if "error" in message:
          error = message["error"]
          if isinstance(error, dict):
            raise RuntimeError(str(error.get("message") or error))
          raise RuntimeError(str(error))
        result = message.get("result")
        if not isinstance(result, dict):
          raise RuntimeError("codex app-server returned a non-object result")
        return result
  finally:
    try:
      if proc.stdin is not None:
        proc.stdin.close()
    except OSError:
      pass
    if proc.poll() is None:
      proc.terminate()
      try:
        proc.wait(timeout=2)
      except subprocess.TimeoutExpired:
        proc.kill()
    else:
      proc.wait(timeout=1)

  detail = "; ".join(line for line in stderr_lines if line)
  raise TimeoutError(f"codex app-server {method} did not return before timeout" + (f": {detail}" if detail else ""))


def _codex_window_from_mapping(window: object) -> dict | None:
  if not isinstance(window, dict):
    return None
  used = window.get("usedPercent", window.get("used_percent"))
  reset = window.get("resetsAt", window.get("reset_at"))
  if used is None or reset is None:
    return None
  try:
    return {
        "used_pct": max(0, min(100, round(float(used)))),
        "reset_epoch": int(reset),
    }
  except (TypeError, ValueError):
    return None


def _codex_limits_from_snapshot(snapshot: object, now: datetime) -> dict | None:
  if not isinstance(snapshot, dict):
    return None
  now_epoch = now.timestamp()
  result = {}
  for window_name, key in (("primary", "primary"), ("secondary", "secondary")):
    window = _codex_window_from_mapping(snapshot.get(window_name))
    if not window or window["reset_epoch"] <= now_epoch:
      continue
    result[key] = {
        "used_pct": window["used_pct"],
        "reset_at": datetime.fromtimestamp(window["reset_epoch"], TZ),
    }
  return result or None


def codex_rate_limits_from_app_server(now: datetime) -> dict | None:
  try:
    response = codex_app_server_rpc("account/rateLimits/read")
  except Exception:
    return None

  snapshots = response.get("rateLimitsByLimitId")
  snapshot = snapshots.get("codex") if isinstance(snapshots, dict) else None
  if snapshot is None:
    snapshot = response.get("rateLimits")
  return _codex_limits_from_snapshot(snapshot, now)


def codex_rate_limits(now: datetime) -> dict | None:
  return codex_rate_limits_from_app_server(now) or codex_rate_limits_from_logs(now)


def _codex_window(name: str, body: str) -> dict | None:
  """Extract one rate-limit window (primary=5h / secondary=weekly) from a log
  line. Handles both snake_case (reset_at/used_percent) and camelCase
  (resetsAt/usedPercent) forms emitted by different Codex layers."""
  match = re.search(r'"' + name + r'"\s*:\s*\{([^}]*)\}', body)
  if not match:
    return None
  seg = match.group(1)
  used = re.search(r'"used_?[pP]ercent"\s*:\s*([0-9.]+)', seg)
  reset = re.search(r'"resets?_?[aA]t"\s*:\s*([0-9]+)', seg)
  if not used or not reset:
    return None
  return _codex_window_from_mapping({
      "used_percent": used.group(1),
      "reset_at": reset.group(1),
  })


def codex_rate_limits_from_logs(now: datetime) -> dict | None:
  """Freshest still-active Codex rate-limit windows from the trace DB.

  The 5h and weekly windows can age differently: a primary window expires every
  few hours, while the secondary weekly reset remains valid for days. Keep each
  future window independently instead of throwing away weekly data just because
  Codex stopped writing fresh primary rows after a limit/exhaustion event.
  """
  try:
    con = sqlite3.connect(f"file:{CODEX_LOGS_DB}?mode=ro&immutable=1", uri=True, timeout=5)
  except sqlite3.Error:
    return None
  try:
    rows = con.execute(
        "SELECT ts, feedback_log_body FROM logs "
        "WHERE feedback_log_body LIKE '%rate_limits%' AND feedback_log_body LIKE '%rimary%' "
        "ORDER BY id DESC LIMIT 15"
    ).fetchall()
  except sqlite3.Error:
    return None
  finally:
    con.close()

  now_epoch = now.timestamp()
  result = {}
  for ts, body in rows:
    _ = ts  # rows are newest-first; reset time decides whether a window is usable.
    for window_name, key in (("primary", "primary"), ("secondary", "secondary")):
      if key in result:
        continue
      window = _codex_window(window_name, body)
      if not window or window["reset_epoch"] <= now_epoch:
        continue
      result[key] = {
          "used_pct": window["used_pct"],
          "reset_at": datetime.fromtimestamp(window["reset_epoch"], TZ),
      }
    if "primary" in result and "secondary" in result:
      break
  return result or None


def codex_usage(now: datetime, week_start: datetime) -> dict:
  # Prefer a direct app-server rate-limit read. The older TUI/log path is kept
  # only as a compatibility fallback because recent Codex versions no longer
  # reliably write fresh rate limits as a startup side effect.
  limits = codex_rate_limits_from_app_server(now)
  tui_usage = None
  if not limits:
    tui_usage = codex_usage_from_tui()
    limits = codex_rate_limits_from_logs(now)
  usage = tui_usage if tui_usage else unknown_usage()
  if limits:
    primary = limits.get("primary")
    if primary:
      usage["h5"] = quota_usage(primary["used_pct"], humanize_delta(primary["reset_at"] - now))
    secondary = limits.get("secondary")
    if secondary:
      usage["weekly"] = quota_usage(secondary["used_pct"], humanize_delta(secondary["reset_at"] - now))
  return usage


def codex_usage_from_tui() -> dict | None:
  session = f"esp32_codex_usage_{os.getpid()}_{int(time.time())}"
  try:
    subprocess.run(["tmux", "kill-session", "-t", session], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    subprocess.run(
        [
            "tmux",
            "new-session",
            "-d",
            "-s",
            session,
            "-x",
            "140",
            "-y",
            "40",
            "cd ~ && TERM=xterm-256color codex --no-alt-screen",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=5,
    )
    time.sleep(float(os.environ.get("DASH_CODEX_TUI_WAIT", "8")))
    capture = subprocess.run(
        ["tmux", "capture-pane", "-t", session, "-p", "-S", "-200"],
        check=True,
        capture_output=True,
        text=True,
        timeout=5,
    ).stdout
  except Exception:
    return None
  finally:
    subprocess.run(["tmux", "kill-session", "-t", session], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)

  match = re.search(r"5h\s+(\d+)%\s+left\s+.*?weekly\s+(\d+)%\s+left", capture, re.IGNORECASE | re.DOTALL)
  if not match:
    return None

  h5_left = max(0, min(100, int(match.group(1))))
  weekly_left = max(0, min(100, int(match.group(2))))
  return {
      "h5": {"used_pct": 100 - h5_left, "reset": "5h"},
      "weekly": {"used_pct": 100 - weekly_left, "reset": next_monday_label()},
  }


def usd_twd(usd: object) -> int:
  return round((safe_float(usd) or 0.0) * USD_TWD)


def daily_series(daily_rows: list, now: datetime, cost_key: str) -> dict:
  """Return a continuous DAILY_CHART_DAYS token series (oldest->newest, gaps=0)
  for the chart, plus the ALL-TIME accumulated token + TWD totals (so the big
  number keeps growing like TokenBar's lifetime total)."""
  by_date = {}
  total_tok = 0
  total_cost = 0.0
  for item in daily_rows:
    tok = int(item.get("totalTokens") or 0)
    cost = safe_float(item.get(cost_key)) or 0.0
    total_tok += tok
    total_cost += cost
    try:
      by_date[datetime.fromisoformat(item.get("date")).date()] = tok
    except (TypeError, ValueError):
      continue

  series = [by_date.get((now - timedelta(days=i)).date(), 0)
            for i in range(DAILY_CHART_DAYS - 1, -1, -1)]
  return {"daily": series, "total_tok": total_tok, "total_twd": usd_twd(total_cost)}


def shorten_model_name(name: str) -> str:
  """Drop vendor prefix / date suffix noise so names fit the device legend."""
  name = re.sub(r"^claude-", "", name)
  return re.sub(r"-\d{8}$", "", name)


def model_mix(daily_rows: list, now: datetime) -> list:
  """Per-model token share over the chart window (last DAILY_CHART_DAYS days).

  Understands both ccusage row shapes: codex uses {"models": {name: stats}},
  claude uses {"modelBreakdowns": [{"modelName": ...}]} (whose entries carry no
  totalTokens, so it is rebuilt from the four token components).
  """
  cutoff = (now - timedelta(days=DAILY_CHART_DAYS - 1)).date()
  totals: dict[str, int] = {}

  def add(name: object, tokens: int) -> None:
    if not name or tokens <= 0:
      return
    key = shorten_model_name(str(name))
    totals[key] = totals.get(key, 0) + tokens

  for item in daily_rows:
    try:
      item_date = datetime.fromisoformat(item.get("date")).date()
    except (TypeError, ValueError):
      continue
    if item_date < cutoff:
      continue
    models = item.get("models")
    if isinstance(models, dict):
      for name, stats in models.items():
        if isinstance(stats, dict):
          add(name, int(safe_float(stats.get("totalTokens")) or 0))
    for entry in item.get("modelBreakdowns") or []:
      if isinstance(entry, dict):
        add(entry.get("modelName"),
            sum(int(safe_float(entry.get(key)) or 0)
                for key in ("inputTokens", "outputTokens",
                            "cacheCreationTokens", "cacheReadTokens")))

  ranked = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)
  if len(ranked) > 3:
    ranked = ranked[:2] + [("other", sum(tok for _, tok in ranked[2:]))]
  total = sum(tok for _, tok in ranked)
  if total <= 0:
    return []
  mix = [{"name": name, "pct": max(1, round(tok * 100 / total)), "tok": tok}
         for name, tok in ranked]
  # Rounding drift lands on the largest share so the stacked bar fills exactly.
  mix[0]["pct"] += 100 - sum(m["pct"] for m in mix)
  return mix


def claude_detail(now: datetime, week_start: datetime) -> dict:
  """Per-window token totals + TWD cost for Claude, plus a daily token series."""
  out = {"h5": {}, "weekly": {}, "daily": [], "total_tok": 0, "total_twd": 0, "models": []}
  try:
    active = ccusage("claude", "blocks", "--active", "--json", "--timezone", TZ_NAME, "--offline")
    for block in active.get("blocks", []):
      if not block.get("isGap"):
        out["h5"] = {"tokens": int(block.get("totalTokens") or 0), "cost_twd": usd_twd(block.get("costUSD"))}
        break
  except Exception:
    pass
  try:
    weekly = ccusage("claude", "weekly", "--json", "--timezone", TZ_NAME, "--offline",
                     "-w", "monday", "--since", week_start.strftime("%Y%m%d"))
    key = week_start.date().isoformat()
    for item in weekly.get("weekly", []):
      if item.get("week") == key:
        out["weekly"] = {"tokens": int(item.get("totalTokens") or 0), "cost_twd": usd_twd(item.get("totalCost"))}
        break
  except Exception:
    pass
  try:
    daily = ccusage("claude", "daily", "--json", "--timezone", TZ_NAME, "--offline")
    out.update(daily_series(daily.get("daily", []), now, "totalCost"))
    out["models"] = model_mix(daily.get("daily", []), now)
  except Exception:
    pass
  return out


def codex_detail(now: datetime, week_start: datetime) -> dict:
  """Per-window token totals + TWD cost for Codex.

  Codex's quota percentage is authoritative from logs_2.sqlite. ccusage is only
  detail data; if it is stale, fail this detail block instead of writing zeros
  that look like real usage.
  """
  out = {"h5": {}, "weekly": {}, "daily": [], "total_tok": 0, "total_twd": 0, "models": []}
  limits = codex_rate_limits(now)
  primary = limits.get("primary") if limits else None
  secondary = limits.get("secondary") if limits else None

  cutoff5 = (
      primary["reset_at"].astimezone(timezone.utc) - timedelta(hours=5)
      if primary else now.astimezone(timezone.utc) - timedelta(hours=5)
  )
  week_cutoff = (
      (secondary["reset_at"] - timedelta(days=7)).date()
      if secondary else week_start.date()
  )

  h5_tokens = 0
  latest_session_at: datetime | None = None
  try:
    sessions = ccusage("codex", "session", "--json", "--timezone", TZ_NAME, "--offline")
    for session in sessions.get("sessions", []):
      last = session.get("lastActivity")
      if not last:
        continue
      last_at = parse_instant(last)
      if latest_session_at is None or last_at > latest_session_at:
        latest_session_at = last_at
      if last_at >= cutoff5:
        h5_tokens += int(session.get("totalTokens") or 0)
  except Exception:
    pass

  week_tokens = 0
  week_cost = 0.0
  daily_rows = []
  latest_daily_date = None
  try:
    daily = ccusage("codex", "daily", "--json", "--timezone", TZ_NAME, "--offline")
    daily_rows = daily.get("daily", [])
    for item in daily_rows:
      try:
        item_date = datetime.fromisoformat(item.get("date")).date()
      except (TypeError, ValueError):
        continue
      if latest_daily_date is None or item_date > latest_daily_date:
        latest_daily_date = item_date
      if item_date >= week_cutoff:
        week_tokens += int(item.get("totalTokens") or 0)
        week_cost += safe_float(item.get("costUSD")) or 0.0
  except Exception:
    pass

  if latest_session_at is None and latest_daily_date is None:
    raise RuntimeError("codex ccusage detail is unavailable")

  # ccusage often lags the live rate-limit windows (it only sees closed
  # sessions/days), so the current window can read 0 tokens while the live
  # percentage shows heavy use. Rather than blank the whole card, surface the
  # freshest real numbers we DO have — the accumulated totals and daily series
  # stay meaningful even when the in-flight window has not been written yet.
  live_h5_used = int(primary.get("used_pct") or 0) if isinstance(primary, dict) else 0
  live_week_used = int(secondary.get("used_pct") or 0) if isinstance(secondary, dict) else 0
  h5_stale = live_h5_used > 0 and h5_tokens == 0
  week_stale = live_week_used > 0 and week_tokens == 0
  if h5_stale:
    print("codex detail: live 5h shows usage but ccusage sessions are stale", file=sys.stderr)
  if week_stale:
    print("codex detail: live weekly shows usage but ccusage daily is stale", file=sys.stderr)

  # Only report a window's tokens/cost when ccusage actually has rows for it.
  # When the live percentage shows usage but ccusage has not caught up, leave the
  # window unset so the device shows "--" (unknown) rather than a false 0. The
  # accumulated totals + daily series below stay populated regardless.
  rate = (week_cost / week_tokens) if week_tokens > 0 else 0.0
  if not h5_stale:
    out["h5"] = {"tokens": h5_tokens, "cost_twd": usd_twd(h5_tokens * rate)}
  if not week_stale:
    out["weekly"] = {"tokens": week_tokens, "cost_twd": usd_twd(week_cost)}
  out.update(daily_series(daily_rows, now, "costUSD"))
  out["models"] = model_mix(daily_rows, now)
  return out


def empty_usage() -> dict:
  return unknown_usage()


def empty_detail() -> dict:
  return {}


def safe_usage(name: str, fn, now: datetime, week_start: datetime) -> tuple[dict, bool]:
  try:
    usage = fn(now, week_start)
    return usage, usage_complete(usage)
  except Exception as exc:
    print(f"{name} usage unavailable: {exc}", file=sys.stderr)
    return empty_usage(), False


def safe_detail(name: str, fn, now: datetime, week_start: datetime) -> tuple[dict, bool]:
  try:
    return fn(now, week_start), True
  except Exception as exc:
    print(f"{name} detail unavailable: {exc}", file=sys.stderr)
    return empty_detail(), False


def hourly_forecast(now: datetime, location: dict | None = None) -> list:
  """Up to 6 upcoming hourly points (hour label, temp, rain %) from open-meteo.
  Fetched independently of the main weather source so it works under CWA too."""
  loc = location or location_payload(CITY, LAT or DEFAULT_LAT, LON or DEFAULT_LON, "default")
  try:
    params = {
        "latitude": loc["lat"],
        "longitude": loc["lon"],
        "hourly": "temperature_2m,precipitation_probability,precipitation",
        "forecast_days": "2",
        "timezone": TZ_NAME,
    }
    url = "https://api.open-meteo.com/v1/forecast?" + urllib.parse.urlencode(params)
    with urllib.request.urlopen(url, timeout=10) as response:
      data = json.loads(response.read().decode("utf-8"))
  except Exception:
    return []

  hourly = data.get("hourly", {})
  times = hourly.get("time") or []
  temps = hourly.get("temperature_2m") or []
  probs = hourly.get("precipitation_probability") or []
  precip = hourly.get("precipitation") or []
  current_hour = now.strftime("%Y-%m-%dT%H")
  start = 0
  for i, t in enumerate(times):
    if str(t)[:13] >= current_hour:
      start = i
      break
  points = []
  for i in range(start, min(start + 6, len(times), len(temps), len(probs))):
    points.append({
        "t": str(times[i])[11:13],
        "temp": round(safe_float(temps[i]) or 0.0),
        "rain": max(0, min(100, int(safe_float(probs[i]) or 0))),
        # Expected rain (mm) for this hour. Used to corroborate the alert; the
        # firmware ignores it.
        "precip": round(safe_float(precip[i]) or 0.0, 1) if i < len(precip) else 0.0,
    })
  return points


def apply_near_term_rain_forecast(weather: dict, hourly: list) -> dict:
  """Promote the dashboard rain chance using the current hour + next N hours.

  Only the tight imminent window (see RAIN_LOOKAHEAD_HOURS) drives the alert, and
  a high probability alone is not enough to flip the card to "Rain soon": the
  model must also expect measurable rain (RAIN_ALERT_PRECIP_MM). Both guards cut
  the false alarms where a dry convective afternoon still reads high-probability.
  """
  lookahead_count = max(1, RAIN_LOOKAHEAD_HOURS + 1)
  near_term = []
  near_points = []
  for point in hourly[:lookahead_count]:
    if isinstance(point, dict):
      value = safe_float(point.get("rain"))
      if value is not None:
        rain_pct = max(0, min(100, int(value)))
        near_term.append(rain_pct)
        precip = safe_float(point.get("precip"))
        near_points.append((rain_pct, max(0.0, precip) if precip is not None else None))
  if not near_term:
    return weather

  current_pct = int(safe_float(weather.get("rain_pct")) or 0)
  imminent_pct = max(near_term)
  weather["rain_pct"] = max(current_pct, imminent_pct)

  # Probability and expected depth must corroborate each other in the same
  # hour. Taking their independent maxima can combine an 80%/0 mm hour with a
  # 20%/2 mm hour and create a false alert. Older hourly payloads without any
  # precip field keep the probability-only fallback.
  has_precip = any(precip is not None for _, precip in near_points)
  forecast_alert = (
      any(rain >= RAIN_ALERT_PCT and precip is not None and precip >= RAIN_ALERT_PRECIP_MM
          for rain, precip in near_points)
      if has_precip
      else imminent_pct >= RAIN_ALERT_PCT
  )
  weather["rain_alert"] = bool(weather.get("is_raining") or forecast_alert)
  if forecast_alert:
    if not weather.get("is_raining") and weather.get("condition") not in {"rain", "thunderstorm"}:
      weather["condition"] = "rain"
      weather["label"] = "Rain soon"
  return weather


def build_dashboard() -> dict:
  now = datetime.now(TZ)
  week_start = (now - timedelta(days=now.weekday())).replace(hour=0, minute=0, second=0, microsecond=0)
  location = weather_location(now)

  weather = read_weather_cache(now)
  if weather is None:
    if CWA_API_KEY:
      try:
        weather = cwa_weather(now, location)
      except Exception:
        weather = None
    if weather is None:
      try:
        weather = open_meteo_weather(location)
      except Exception:
        weather = None
    if weather is not None:
      write_weather_cache(now, weather)
    else:
      weather = fallback_weather()
  weather = normalize_weather(weather)
  weather["weekday"] = now.strftime("%a")
  weather["day"] = str(now.day)
  weather["month"] = now.strftime("%b")
  weather["year"] = str(now.year)
  weather["date"] = f"{weather['month']} {weather['day']}"
  weather["hourly"] = hourly_forecast(now, location)
  apply_near_term_rain_forecast(weather, weather["hourly"])
  weather.update(sun_and_air(location))
  weather["city_zh"] = localized_city_name(location)

  claude, claude_ok = safe_usage("claude", claude_usage, now, week_start)
  codex, codex_ok = safe_usage("codex", codex_usage, now, week_start)
  for usage in (claude, codex):
    for window in ("h5", "weekly"):
      block = usage.get(window)
      if isinstance(block, dict) and "reset" in block:
        block["reset"] = reset_countdown(block["reset"], now)

  # Attach per-window token totals + API-equivalent TWD cost, plus the daily
  # token series + accumulated totals (detail-page bar chart).
  claude_cost, claude_detail_ok = safe_detail("claude", claude_detail, now, week_start)
  codex_cost, codex_detail_ok = safe_detail("codex", codex_detail, now, week_start)
  for usage, detail, detail_ok in (
      (claude, claude_cost, claude_detail_ok),
      (codex, codex_cost, codex_detail_ok),
  ):
    if not detail_ok:
      continue
    for window in ("h5", "weekly"):
      if isinstance(usage.get(window), dict):
        usage[window].update(detail.get(window, {}))
    if "daily" in detail:
      usage["daily"] = detail["daily"]
    if "total_tok" in detail:
      usage["total_tok"] = detail["total_tok"]
    if "total_twd" in detail:
      usage["total_twd"] = detail["total_twd"]
    if "models" in detail:
      usage["models"] = detail["models"]

  quota_ok = claude_ok and codex_ok and claude_detail_ok and codex_detail_ok
  quota_source = "live" if quota_ok else ("fallback" if not claude_ok and not codex_ok else "partial")

  return {
      "weather": weather,
      "claude": claude,
      "codex": codex,
      "meta": {
          "source": "server dashboard",
          "quota_source": quota_source,
          "generated_at": now.isoformat(),
          "timezone": TZ_NAME,
      },
  }


def atomic_write_json(path: Path, payload: dict) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as temp:
    json.dump(payload, temp, ensure_ascii=True, separators=(",", ":"))
    temp.write("\n")
    temp_path = Path(temp.name)
  temp_path.replace(path)
  path.chmod(0o644)


def main() -> int:
  atomic_write_json(OUTPUT_PATH, build_dashboard())
  print(f"wrote {OUTPUT_PATH}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
