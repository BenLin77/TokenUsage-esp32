# CLAUDE.md

Project guidance for Claude Code. To avoid drift, the full architecture and
change guide lives in **[`AGENTS.md`](AGENTS.md)** — read it first.

## TL;DR for this repo

- ESP32-S3 dashboard firmware (`firmware/src/`) + a Python collector (`server/`). The
  device is a thin client that renders one `dashboard.json` fetched over HTTP.
- **All app logic is in `firmware/src/main.cpp`**; hardware/pin map is in
  `firmware/src/board_config.h`.
- **Draw through `gfx` (the PSRAM back buffer), then call `present()`.** Never
  draw on `lcd` directly in app code. Device-only calls (`init`, `setRotation`,
  `setBrightness`, `touch`, `getTouch`) use `lcd`.
- Fetch (120 s) and render (1 s) are decoupled — see the loop cadence in
  `AGENTS.md`.
- Verify firmware changes with `.venv/bin/pio run -d firmware` (there is no hardware in CI).
  Build an `lcd-smoke-*` env too after touching `board_config.h`.
- Keep the JSON contract in sync across `main.cpp`, the collector, and
  `server/dashboard.sample.json` when changing weather types or quota fields.

See `AGENTS.md` for the render pipeline, API contract, layout coordinates, and
collector details.
