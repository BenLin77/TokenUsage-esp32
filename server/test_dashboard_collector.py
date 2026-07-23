#!/usr/bin/env python3
"""Regression tests for the dashboard collector."""

from __future__ import annotations

import json
import unittest
import sys
import tempfile
from datetime import datetime, timedelta
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dashboard_collector as collector


class CollectorReliabilityTests(unittest.TestCase):
  def test_ccusage_defaults_to_a_pinned_package(self) -> None:
    calls: list[list[str]] = []

    def fake_run_json(args: list[str], timeout: int = 120) -> dict:
      calls.append(args)
      return {}

    with patch.object(collector, "run_json", fake_run_json):
      collector.ccusage("claude", "daily", "--json")

    self.assertEqual(["npx", "-y", collector.CCUSAGE_NPX_PACKAGE], calls[0][:3])
    self.assertNotIn("@latest", calls[0][2])

  def test_build_dashboard_survives_usage_failures(self) -> None:
    now = datetime(2026, 7, 1, 12, 0, tzinfo=collector.TZ)

    with patch.object(collector, "datetime") as fake_datetime, \
         patch.object(collector, "read_weather_cache", return_value=None), \
         patch.object(collector, "open_meteo_weather", side_effect=RuntimeError("weather down")), \
         patch.object(collector, "hourly_forecast", return_value=[]), \
         patch.object(collector, "sun_and_air", return_value={}), \
         patch.object(collector, "localized_city_name", return_value=""), \
         patch.object(collector, "claude_usage", side_effect=RuntimeError("claude down")), \
         patch.object(collector, "codex_usage", side_effect=RuntimeError("codex down")), \
         patch.object(collector, "claude_detail", side_effect=RuntimeError("detail down")), \
         patch.object(collector, "codex_detail", side_effect=RuntimeError("detail down")):
      fake_datetime.now.return_value = now
      fake_datetime.fromisoformat.side_effect = datetime.fromisoformat
      fake_datetime.strptime.side_effect = datetime.strptime

      payload = collector.build_dashboard()

    self.assertEqual({"h5", "weekly"}, set(payload["claude"]))
    self.assertEqual({"h5", "weekly"}, set(payload["codex"]))
    self.assertNotIn("used_pct", payload["claude"]["h5"])
    self.assertEqual("unavailable", payload["claude"]["h5"]["status"])
    self.assertEqual("--", payload["claude"]["h5"]["reset"])
    self.assertNotIn("used_pct", payload["codex"]["weekly"])
    self.assertEqual("unavailable", payload["codex"]["weekly"]["status"])
    self.assertEqual("--", payload["codex"]["weekly"]["reset"])
    self.assertEqual("fallback", payload["meta"]["quota_source"])

  def test_claude_ccusage_without_data_returns_unknown_windows(self) -> None:
    now = datetime(2026, 7, 1, 12, 0, tzinfo=collector.TZ)
    week_start = datetime(2026, 6, 29, tzinfo=collector.TZ)

    with patch.object(collector, "claude_usage_from_tui", return_value=None), \
         patch.object(collector, "ccusage", return_value={}):
      usage = collector.claude_usage(now, week_start)

    self.assertNotIn("used_pct", usage["h5"])
    self.assertEqual("unavailable", usage["h5"]["status"])
    self.assertNotIn("used_pct", usage["weekly"])
    self.assertEqual("unavailable", usage["weekly"]["status"])

  def test_near_term_forecast_promotes_dashboard_rain_alert(self) -> None:
    weather = {
        "city": "Zhonghe",
        "temp_c": 33,
        "condition": "partly_cloudy",
        "label": "Partly cloudy",
        "rain_pct": 10,
        "rain_mm": 0.0,
        "is_raining": False,
        "rain_alert": False,
    }
    hourly = [
        {"t": "14", "temp": 31, "rain": 76, "precip": 0.8},
        {"t": "15", "temp": 31, "rain": 89, "precip": 2.1},
        {"t": "16", "temp": 32, "rain": 95, "precip": 3.0},
        {"t": "17", "temp": 31, "rain": 90, "precip": 2.5},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    # Only the imminent window (current + next hour) drives the alert.
    self.assertEqual(89, weather["rain_pct"])
    self.assertTrue(weather["rain_alert"])
    self.assertFalse(weather["is_raining"])
    self.assertEqual("rain", weather["condition"])
    self.assertEqual("Rain soon", weather["label"])

  def test_near_term_forecast_does_not_alert_on_high_prob_but_no_precip(self) -> None:
    weather = {
        "city": "Zhonghe",
        "temp_c": 34,
        "condition": "partly_cloudy",
        "label": "Partly cloudy",
        "rain_pct": 20,
        "rain_mm": 0.0,
        "is_raining": False,
        "rain_alert": False,
    }
    # Dry convective afternoon: high probability but the model expects no
    # measurable rain, so the card must stay off the "Rain soon" alert.
    hourly = [
        {"t": "14", "temp": 34, "rain": 80, "precip": 0.0},
        {"t": "15", "temp": 34, "rain": 85, "precip": 0.0},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    self.assertEqual(85, weather["rain_pct"])
    self.assertFalse(weather["rain_alert"])
    self.assertEqual("partly_cloudy", weather["condition"])
    self.assertEqual("Partly cloudy", weather["label"])

  def test_near_term_forecast_clears_uncorroborated_provider_alert(self) -> None:
    weather = collector.normalize_weather({
        "city": "Zhonghe",
        "temp_c": 34,
        "condition": "partly_cloudy",
        "label": "Partly cloudy",
        "rain_pct": 75,
        "rain_mm": 0.0,
        "is_raining": False,
        "rain_alert": False,
    })
    self.assertTrue(weather["rain_alert"])
    hourly = [
        {"t": "14", "temp": 34, "rain": 75, "precip": 0.0},
        {"t": "15", "temp": 34, "rain": 80, "precip": 0.0},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    self.assertFalse(weather["rain_alert"])
    self.assertEqual("partly_cloudy", weather["condition"])

  def test_near_term_forecast_does_not_mix_probability_and_rain_across_hours(self) -> None:
    weather = {
        "city": "Zhonghe",
        "temp_c": 34,
        "condition": "partly_cloudy",
        "label": "Partly cloudy",
        "rain_pct": 20,
        "rain_mm": 0.0,
        "is_raining": False,
        "rain_alert": False,
    }
    hourly = [
        {"t": "14", "temp": 34, "rain": 80, "precip": 0.0},
        {"t": "15", "temp": 34, "rain": 20, "precip": 2.0},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    self.assertFalse(weather["rain_alert"])
    self.assertEqual("partly_cloudy", weather["condition"])

  def test_near_term_forecast_keeps_observed_rain_alert(self) -> None:
    weather = {
        "city": "Zhonghe",
        "temp_c": 29,
        "condition": "rain",
        "label": "Rain",
        "rain_pct": 20,
        "rain_mm": 0.3,
        "is_raining": True,
        "rain_alert": True,
    }
    hourly = [
        {"t": "14", "temp": 29, "rain": 20, "precip": 0.0},
        {"t": "15", "temp": 29, "rain": 20, "precip": 0.0},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    self.assertTrue(weather["rain_alert"])
    self.assertEqual("rain", weather["condition"])

  def test_near_term_forecast_without_precip_field_keeps_probability_fallback(self) -> None:
    weather = {
        "city": "Zhonghe",
        "temp_c": 32,
        "condition": "partly_cloudy",
        "label": "Partly cloudy",
        "rain_pct": 20,
        "rain_mm": 0.0,
        "is_raining": False,
        "rain_alert": False,
    }
    hourly = [
        {"t": "14", "temp": 32, "rain": 75},
        {"t": "15", "temp": 31, "rain": 80},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    self.assertTrue(weather["rain_alert"])
    self.assertEqual("rain", weather["condition"])
    self.assertEqual("Rain soon", weather["label"])

  def test_near_term_forecast_ignores_spike_beyond_imminent_window(self) -> None:
    weather = {
        "city": "Zhonghe",
        "temp_c": 33,
        "condition": "partly_cloudy",
        "label": "Partly cloudy",
        "rain_pct": 10,
        "rain_mm": 0.0,
        "is_raining": False,
        "rain_alert": False,
    }
    # A spike three hours out must not flip the card now.
    hourly = [
        {"t": "14", "temp": 33, "rain": 20, "precip": 0.0},
        {"t": "15", "temp": 33, "rain": 25, "precip": 0.0},
        {"t": "16", "temp": 32, "rain": 30, "precip": 0.1},
        {"t": "17", "temp": 31, "rain": 95, "precip": 4.0},
    ]

    collector.apply_near_term_rain_forecast(weather, hourly)

    self.assertEqual(25, weather["rain_pct"])
    self.assertFalse(weather["rain_alert"])
    self.assertEqual("partly_cloudy", weather["condition"])

  def test_cwa_condition_keeps_thunderstorm_when_precip_allowed(self) -> None:
    condition, label = collector.cwa_condition_from_text("午後雷陣雨", allow_precip=True)
    self.assertEqual("thunderstorm", condition)
    self.assertEqual("Thunderstorm", label)

  def test_cwa_condition_downgrades_dry_thunderstorm_forecast(self) -> None:
    # A dry, low-probability "午後雷陣雨" reads through to its cloudy sky state
    # instead of painting a thunderstorm icon on a sunny afternoon.
    condition, _ = collector.cwa_condition_from_text("多雲午後短暫雷陣雨", allow_precip=False)
    self.assertEqual("partly_cloudy", condition)
    bare, _ = collector.cwa_condition_from_text("午後雷陣雨", allow_precip=False)
    self.assertEqual("cloudy", bare)

  def test_cwa_weather_downgrades_condition_on_dry_low_prob_forecast(self) -> None:
    now = datetime(2026, 7, 3, 12, 15, tzinfo=collector.TZ)
    forecast = {
        "records": {
            "location": [
                {
                    "weatherElement": [
                        {"elementName": "Wx", "time": [{
                            "startTime": "2026-07-03 12:00:00",
                            "endTime": "2026-07-03 18:00:00",
                            "parameter": {"parameterName": "午後短暫雷陣雨"},
                        }]},
                        {"elementName": "PoP", "time": [{
                            "startTime": "2026-07-03 12:00:00",
                            "endTime": "2026-07-03 18:00:00",
                            "parameter": {"parameterName": "30"},
                        }]},
                    ]
                }
            ]
        }
    }
    with patch.object(collector, "cwa_fetch", return_value=forecast), \
         patch.object(collector, "cwa_observation", return_value=(34, 0.0)):
      weather = collector.cwa_weather(now, {"city": "Zhonghe"})

    self.assertEqual("cloudy", weather["condition"])
    self.assertFalse(weather["is_raining"])
    self.assertFalse(weather["rain_alert"])
    self.assertEqual(30, weather["rain_pct"])

  def test_cwa_weather_keeps_thunderstorm_when_high_probability(self) -> None:
    now = datetime(2026, 7, 3, 12, 15, tzinfo=collector.TZ)
    forecast = {
        "records": {
            "location": [
                {
                    "weatherElement": [
                        {"elementName": "Wx", "time": [{
                            "startTime": "2026-07-03 12:00:00",
                            "endTime": "2026-07-03 18:00:00",
                            "parameter": {"parameterName": "午後短暫雷陣雨"},
                        }]},
                        {"elementName": "PoP", "time": [{
                            "startTime": "2026-07-03 12:00:00",
                            "endTime": "2026-07-03 18:00:00",
                            "parameter": {"parameterName": "70"},
                        }]},
                    ]
                }
            ]
        }
    }
    with patch.object(collector, "cwa_fetch", return_value=forecast), \
         patch.object(collector, "cwa_observation", return_value=(31, 0.0)):
      weather = collector.cwa_weather(now, {"city": "Zhonghe"})

    self.assertEqual("thunderstorm", weather["condition"])

  def _township_payload(self) -> dict:
    return {
        "records": {
            "Locations": [
                {
                    "LocationsName": "新北市",
                    "Location": [
                        {
                            "LocationName": "中和區",
                            "WeatherElement": [
                                {"ElementName": "天氣現象", "Time": [
                                    {"StartTime": "2026-07-03T09:00:00+08:00",
                                     "EndTime": "2026-07-03T12:00:00+08:00",
                                     "ElementValue": [{"Weather": "晴", "WeatherCode": "01"}]},
                                    {"StartTime": "2026-07-03T12:00:00+08:00",
                                     "EndTime": "2026-07-03T15:00:00+08:00",
                                     "ElementValue": [{"Weather": "午後短暫雷陣雨", "WeatherCode": "15"}]},
                                ]},
                                {"ElementName": "3小時降雨機率", "Time": [
                                    {"StartTime": "2026-07-03T09:00:00+08:00",
                                     "EndTime": "2026-07-03T12:00:00+08:00",
                                     "ElementValue": [{"ProbabilityOfPrecipitation": "0"}]},
                                    {"StartTime": "2026-07-03T12:00:00+08:00",
                                     "EndTime": "2026-07-03T15:00:00+08:00",
                                     "ElementValue": [{"ProbabilityOfPrecipitation": "30"}]},
                                ]},
                                {"ElementName": "溫度", "Time": [
                                    {"DataTime": "2026-07-03T12:00:00+08:00",
                                     "ElementValue": [{"Temperature": "35"}]},
                                    {"DataTime": "2026-07-03T13:00:00+08:00",
                                     "ElementValue": [{"Temperature": "34"}]},
                                ]},
                            ],
                        },
                    ],
                }
            ]
        }
    }

  def test_township_forecast_reads_district_slot(self) -> None:
    now = datetime(2026, 7, 3, 12, 30, tzinfo=collector.TZ)
    with patch.object(collector, "CWA_TOWN", "中和區"), \
         patch.object(collector, "cwa_fetch", return_value=self._township_payload()):
      wx_text, rain_pct, temp_c = collector.cwa_township_forecast(now)
    self.assertEqual("午後短暫雷陣雨", wx_text)
    self.assertEqual(30, rain_pct)
    self.assertEqual(35, temp_c)

  def test_cwa_weather_uses_township_and_downgrades_dry_forecast(self) -> None:
    now = datetime(2026, 7, 3, 12, 30, tzinfo=collector.TZ)
    with patch.object(collector, "CWA_TOWN", "中和區"), \
         patch.object(collector, "cwa_fetch", return_value=self._township_payload()), \
         patch.object(collector, "cwa_observation", return_value=(34, 0.0)):
      weather = collector.cwa_weather(now, {"city": "Zhonghe"})
    # 午後短暫雷陣雨 at 30% and dry -> icon downgraded to its sky state, not rain.
    self.assertEqual("cloudy", weather["condition"])
    self.assertEqual(30, weather["rain_pct"])
    self.assertFalse(weather["is_raining"])
    self.assertEqual(34, weather["temp_c"])

  def test_cwa_weather_township_temp_falls_back_when_station_missing(self) -> None:
    now = datetime(2026, 7, 3, 12, 30, tzinfo=collector.TZ)
    with patch.object(collector, "CWA_TOWN", "中和區"), \
         patch.object(collector, "cwa_fetch", return_value=self._township_payload()), \
         patch.object(collector, "cwa_observation", return_value=(None, 0.0)):
      weather = collector.cwa_weather(now, {"city": "Zhonghe"})
    self.assertEqual(35, weather["temp_c"])  # from township 溫度, not the 27 default

  def test_sun_time_matches_taipei_almanac(self) -> None:
    day = datetime(2026, 7, 3, 12, 0, tzinfo=collector.TZ)
    self.assertEqual("05:08", collector.sun_time(25.033, 121.565, day, 8, True))
    self.assertEqual("18:48", collector.sun_time(25.033, 121.565, day, 8, False))

  def test_sun_and_air_computes_sun_locally_and_reads_uv(self) -> None:
    payload = json.dumps({"current": {"us_aqi": 88, "uv_index": 10.2}}).encode()

    class _Resp:
      def __enter__(self): return self
      def __exit__(self, *a): return False
      def read(self): return payload

    with patch.object(collector, "CITY", "Zhonghe"), \
         patch.object(collector.urllib.request, "urlopen", return_value=_Resp()):
      out = collector.sun_and_air({"lat": "25.0330", "lon": "121.5654"})

    self.assertEqual(88, out["aqi"])
    self.assertEqual(10, out["uv"])
    self.assertIn("sunrise", out)
    self.assertIn("sunset", out)
    self.assertRegex(out["sunrise"], r"^\d\d:\d\d$")

  def test_weather_location_prefers_fixed_coordinates_before_auto_ip(self) -> None:
    now = datetime(2026, 7, 1, 14, 15, tzinfo=collector.TZ)

    with patch.object(collector, "CITY", "Zhonghe"), \
         patch.object(collector, "LAT", "25.0330"), \
         patch.object(collector, "LON", "121.5654"), \
         patch.object(collector, "AUTO_LOCATION", True), \
         patch.object(collector, "ip_geolocation", side_effect=AssertionError("fixed coordinates must win")):
      location = collector.weather_location(now)

    self.assertEqual("fixed", location["source"])
    self.assertEqual("Zhonghe", location["city"])
    self.assertEqual("25.0330", location["lat"])
    self.assertEqual("121.5654", location["lon"])

  def test_latest_esp32_client_ip_from_log(self) -> None:
    log = (
        '198.51.100.10 - - [01/Jul/2026:14:10:11 +0800] "GET /dashboard.json HTTP/1.1" 200 885 "-" "curl/8.5.0"\n'
        '203.0.113.7 - - [01/Jul/2026:14:12:10 +0800] "GET /dashboard.json HTTP/1.1" 200 885 "-" "ESP32HTTPClient"\n'
    )
    with tempfile.TemporaryDirectory() as tempdir:
      path = Path(tempdir) / "access.log"
      path.write_text(log, encoding="utf-8")
      with patch.object(collector, "ESP32_ACCESS_LOG", path):
        self.assertEqual("203.0.113.7", collector.latest_esp32_client_ip())

  def test_codex_rate_limits_keep_future_weekly_when_primary_expired(self) -> None:
    now = datetime(2026, 7, 1, 13, 38, tzinfo=collector.TZ)
    expired_primary = int((now - timedelta(minutes=7)).timestamp())
    future_secondary = int((now + timedelta(days=5, hours=21)).timestamp())
    body = (
        '{"rate_limits":{"primary":{"used_percent":100,"reset_at":%d},'
        '"secondary":{"used_percent":47,"reset_at":%d}}}'
    ) % (expired_primary, future_secondary)

    with tempfile.TemporaryDirectory() as tempdir:
      db_path = Path(tempdir) / "logs.sqlite"
      import sqlite3

      con = sqlite3.connect(db_path)
      con.execute("CREATE TABLE logs (id INTEGER PRIMARY KEY, ts REAL, feedback_log_body TEXT)")
      con.execute("INSERT INTO logs (ts, feedback_log_body) VALUES (?, ?)", ((now - timedelta(hours=1)).timestamp(), body))
      con.commit()
      con.close()

      with patch.object(collector, "CODEX_LOGS_DB", db_path):
        limits = collector.codex_rate_limits_from_logs(now)

    self.assertIsNotNone(limits)
    self.assertNotIn("primary", limits)
    self.assertEqual(47, limits["secondary"]["used_pct"])
    self.assertEqual(datetime.fromtimestamp(future_secondary, collector.TZ), limits["secondary"]["reset_at"])

  def test_codex_rate_limits_from_app_server_parses_codex_bucket(self) -> None:
    now = datetime(2026, 7, 1, 14, 8, tzinfo=collector.TZ)
    primary_reset = int((now + timedelta(hours=4, minutes=42)).timestamp())
    weekly_reset = int((now + timedelta(days=5, hours=20)).timestamp())
    response = {
        "rateLimits": {"limitId": "legacy", "primary": {"usedPercent": 99, "resetsAt": primary_reset}},
        "rateLimitsByLimitId": {
            "codex": {
                "limitId": "codex",
                "primary": {"usedPercent": 57, "windowDurationMins": 300, "resetsAt": primary_reset},
                "secondary": {"usedPercent": 56, "windowDurationMins": 10080, "resetsAt": weekly_reset},
            }
        },
        "rateLimitResetCredits": {"availableCount": 1},
    }

    with patch.object(collector, "codex_app_server_rpc", return_value=response):
      limits = collector.codex_rate_limits_from_app_server(now)

    self.assertIsNotNone(limits)
    self.assertEqual(57, limits["primary"]["used_pct"])
    self.assertEqual(datetime.fromtimestamp(primary_reset, collector.TZ), limits["primary"]["reset_at"])
    self.assertEqual(56, limits["secondary"]["used_pct"])
    self.assertEqual(datetime.fromtimestamp(weekly_reset, collector.TZ), limits["secondary"]["reset_at"])

  def test_codex_usage_prefers_app_server_without_tui_warmup(self) -> None:
    now = datetime(2026, 7, 1, 14, 8, tzinfo=collector.TZ)
    week_start = datetime(2026, 6, 29, tzinfo=collector.TZ)
    limits = {
        "primary": {"used_pct": 57, "reset_at": now + timedelta(hours=4, minutes=42)},
        "secondary": {"used_pct": 56, "reset_at": now + timedelta(days=5, hours=20)},
    }

    def must_not_warm_tui() -> None:
      raise AssertionError("direct app-server rate limits should not launch Codex TUI")

    with patch.object(collector, "codex_rate_limits_from_app_server", return_value=limits), \
         patch.object(collector, "codex_usage_from_tui", must_not_warm_tui), \
         patch.object(collector, "codex_rate_limits_from_logs", side_effect=AssertionError("logs should not be needed")):
      usage = collector.codex_usage(now, week_start)

    self.assertEqual({"used_pct": 57, "reset": "4h42m"}, usage["h5"])
    self.assertEqual({"used_pct": 56, "reset": "5d20h"}, usage["weekly"])

  def test_codex_usage_does_not_zero_unknown_5h_when_weekly_is_known(self) -> None:
    now = datetime(2026, 7, 1, 13, 38, tzinfo=collector.TZ)
    week_start = datetime(2026, 6, 29, tzinfo=collector.TZ)
    limits = {
        "secondary": {"used_pct": 47, "reset_at": now + timedelta(days=5, hours=21)},
    }

    def ccusage_must_not_be_used(*_args: str) -> dict:
      raise AssertionError("codex quota must not fall back to ccusage token totals")

    with patch.object(collector, "codex_rate_limits_from_app_server", return_value=None), \
         patch.object(collector, "codex_usage_from_tui", return_value=None), \
         patch.object(collector, "codex_rate_limits_from_logs", return_value=limits), \
         patch.object(collector, "ccusage", ccusage_must_not_be_used):
      usage = collector.codex_usage(now, week_start)

    self.assertNotIn("used_pct", usage["h5"])
    self.assertEqual("unavailable", usage["h5"]["status"])
    self.assertEqual(47, usage["weekly"]["used_pct"])
    self.assertEqual("5d21h", usage["weekly"]["reset"])

  def test_codex_detail_degrades_to_best_available_when_ccusage_is_stale(self) -> None:
    # ccusage lags the live window: the current 5h/weekly buckets read 0 tokens
    # while the live percentage shows usage. The card must still show the real
    # accumulated totals instead of blanking (the previous "raise" behavior).
    now = datetime(2026, 7, 1, 12, 0, tzinfo=collector.TZ)
    week_start = datetime(2026, 6, 29, tzinfo=collector.TZ)

    def fake_ccusage(*args: str) -> dict:
      if args[1] == "session":
        return {"sessions": [{"lastActivity": "2026-06-26T07:58:11.546Z", "totalTokens": 4450830}]}
      if args[1] == "daily":
        return {"daily": [{"date": "2026-06-26", "totalTokens": 4450830, "costUSD": 5.84,
                           "models": {"gpt-5.5": {"totalTokens": 4450830}}}]}
      return {}

    limits = {
        "primary": {"used_pct": 65, "reset_at": now + timedelta(hours=1, minutes=31)},
        "secondary": {"used_pct": 41, "reset_at": now + timedelta(days=5, hours=22)},
    }
    with patch.object(collector, "codex_rate_limits_from_app_server", return_value=None), \
         patch.object(collector, "codex_rate_limits_from_logs", return_value=limits), \
         patch.object(collector, "ccusage", fake_ccusage):
      detail = collector.codex_detail(now, week_start)

    # In-flight windows have no fresh ccusage rows yet, so they stay unset
    # (device shows "--", not a false 0), but the accumulated totals reflect the
    # real (older) usage rather than blanking the whole card.
    self.assertNotIn("tokens", detail["h5"])
    self.assertNotIn("tokens", detail["weekly"])
    self.assertEqual(4450830, detail["total_tok"])
    self.assertGreater(detail["total_twd"], 0)
    self.assertEqual([{"name": "gpt-5.5", "pct": 100, "tok": 4450830}], detail["models"])

  def test_model_mix_reads_both_ccusage_shapes_and_caps_at_three(self) -> None:
    now = datetime(2026, 7, 1, 12, 0, tzinfo=collector.TZ)
    rows = [
        # codex shape: models dict keyed by name.
        {"date": "2026-06-30", "models": {
            "gpt-5.5": {"totalTokens": 700},
            "gpt-5.3-codex-spark": {"totalTokens": 200},
        }},
        # claude shape: modelBreakdowns list without totalTokens.
        {"date": "2026-07-01", "modelBreakdowns": [
            {"modelName": "claude-opus-4-8", "inputTokens": 20, "outputTokens": 30,
             "cacheCreationTokens": 0, "cacheReadTokens": 10},
            {"modelName": "claude-haiku-4-5-20251001", "inputTokens": 40, "outputTokens": 0,
             "cacheCreationTokens": 0, "cacheReadTokens": 0},
        ]},
        # outside the chart window: ignored.
        {"date": "2026-01-01", "models": {"gpt-old": {"totalTokens": 999999}}},
    ]

    mix = collector.model_mix(rows, now)

    self.assertEqual(3, len(mix))
    self.assertEqual({"name": "gpt-5.5", "pct": 70, "tok": 700}, mix[0])
    self.assertEqual({"name": "gpt-5.3-codex-spark", "pct": 20, "tok": 200}, mix[1])
    # Remaining models merge into "other"; names lose vendor/date noise first.
    self.assertEqual({"name": "other", "pct": 10, "tok": 100}, mix[2])
    self.assertEqual(100, sum(m["pct"] for m in mix))

  def test_model_mix_shortens_names_and_returns_empty_without_data(self) -> None:
    now = datetime(2026, 7, 1, 12, 0, tzinfo=collector.TZ)
    self.assertEqual([], collector.model_mix([], now))
    mix = collector.model_mix(
        [{"date": "2026-07-01", "modelBreakdowns": [
            {"modelName": "claude-haiku-4-5-20251001", "inputTokens": 5, "outputTokens": 5,
             "cacheCreationTokens": 0, "cacheReadTokens": 0}]}], now)
    self.assertEqual([{"name": "haiku-4-5", "pct": 100, "tok": 10}], mix)

  def test_build_dashboard_omits_failed_detail_without_zeroing_usage(self) -> None:
    now = datetime(2026, 7, 1, 12, 0, tzinfo=collector.TZ)
    codex_usage = {
        "h5": {"used_pct": 65, "reset": "1h31m"},
        "weekly": {"used_pct": 41, "reset": "5d22h"},
    }

    with patch.object(collector, "datetime") as fake_datetime, \
         patch.object(collector, "read_weather_cache", return_value=None), \
         patch.object(collector, "open_meteo_weather", return_value=collector.fallback_weather()), \
         patch.object(collector, "hourly_forecast", return_value=[]), \
         patch.object(collector, "sun_and_air", return_value={"aqi": 42, "uv": 8, "sunrise": "05:08", "sunset": "18:48"}), \
         patch.object(collector, "localized_city_name", return_value="中和區"), \
         patch.object(collector, "claude_usage", return_value={"h5": {"used_pct": 1, "reset": "1h"}, "weekly": {"used_pct": 2, "reset": "2d"}}), \
         patch.object(collector, "claude_detail", return_value={"h5": {"tokens": 10}, "weekly": {"tokens": 20}, "daily": [10], "total_tok": 30, "total_twd": 1}), \
         patch.object(collector, "codex_usage", return_value=codex_usage), \
         patch.object(collector, "codex_detail", side_effect=RuntimeError("stale detail")):
      fake_datetime.now.return_value = now
      fake_datetime.fromisoformat.side_effect = datetime.fromisoformat
      fake_datetime.strptime.side_effect = datetime.strptime

      payload = collector.build_dashboard()

    self.assertEqual(65, payload["codex"]["h5"]["used_pct"])
    self.assertNotIn("tokens", payload["codex"]["h5"])
    self.assertNotIn("daily", payload["codex"])
    self.assertEqual("partial", payload["meta"]["quota_source"])
    self.assertEqual(42, payload["weather"]["aqi"])
    self.assertEqual("18:48", payload["weather"]["sunset"])
    self.assertEqual("中和區", payload["weather"]["city_zh"])

  def test_bare_date_reset_on_reset_day_does_not_roll_a_year(self) -> None:
    # The Claude weekly reset arrives as a bare date (e.g. "Jul 3"). On the
    # reset day itself the date is anchored at midnight, which is already behind
    # `now` — it must NOT roll a full year forward (was reported as "364d15h").
    now = datetime(2026, 7, 3, 8, 45, tzinfo=collector.TZ)
    self.assertEqual("soon", collector.reset_countdown("Jul 3", now))
    # Future dates still count down normally; genuinely past dates roll forward.
    self.assertEqual("2d15h", collector.reset_countdown("Jul 6", now))
    self.assertEqual("361d15h", collector.reset_countdown("Jun 30", now))

  def test_weekly_reset_keeps_date_and_time_for_exact_countdown(self) -> None:
    # The real weekly TUI line is "Resets Jul 3, 4:59pm (Asia/Taipei)". The time
    # must survive compaction so a same-day reset shows real hours, not "soon".
    now = datetime(2026, 7, 3, 9, 15, tzinfo=collector.TZ)
    compact = collector.compact_tui_reset("Jul 3, 4:59pm (Asia/Taipei)")
    self.assertEqual("Jul 3, 4:59pm", compact)
    self.assertEqual("7h44m", collector.reset_countdown(compact, now))
    # Next-week resets and the session clock line still work.
    self.assertEqual("7d7h", collector.reset_countdown("Jul 10, 4:59pm", now))
    self.assertEqual("1h54m", collector.reset_countdown(
        collector.compact_tui_reset("11:09am (Asia/Taipei)"), now))
    # A yearful form still drops the year (falls back to a bare date).
    self.assertEqual("Jul 3", collector.compact_tui_reset("Jul 3, 2026"))


if __name__ == "__main__":
  unittest.main()
