#!/usr/bin/env python3
"""Tiny local JSON server for the ESP32 dashboard firmware."""

from __future__ import annotations

import argparse
import json
import math
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def build_payload() -> dict:
    now = time.localtime()
    wave = (math.sin(time.time() / 45.0) + 1.0) / 2.0
    temp_c = 25 + round(wave * 5)
    rain_pct = round(20 + wave * 55)
    is_raining = rain_pct >= 70
    rain_mm = round((rain_pct - 65) / 25, 1) if is_raining else 0.0

    return {
        "weather": {
            "city": "Taipei",
            "temp_c": temp_c,
            "condition": "partly_cloudy" if rain_pct < 55 else "rain",
            "label": "Partly cloudy" if rain_pct < 55 else "Light rain",
            "rain_pct": rain_pct,
            "rain_mm": rain_mm,
            "is_raining": is_raining,
            "rain_alert": is_raining or rain_pct >= 60,
            "weekday": time.strftime("%a", now),
            "day": str(now.tm_mday),
            "month": time.strftime("%b", now),
            "year": str(now.tm_year),
            "date": time.strftime("%b ", now) + str(now.tm_mday),
        },
        "claude": {
            "h5": {"used_pct": 28, "reset": time.strftime("%H:%M", now)},
            "weekly": {"used_pct": 59, "reset": "Mon"},
        },
        "codex": {
            "h5": {"used_pct": 12, "reset": time.strftime("%H:%M", now)},
            "weekly": {"used_pct": 37, "reset": "Mon"},
        },
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path not in {"/", "/dashboard.json"}:
            self.send_error(404)
            return

        body = json.dumps(build_payload(), separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        print("%s - %s" % (self.address_string(), fmt % args))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Serving dashboard JSON on http://{args.host}:{args.port}/dashboard.json")
    server.serve_forever()


if __name__ == "__main__":
    main()
