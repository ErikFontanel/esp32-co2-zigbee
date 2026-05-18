# Project notes

## Verifying fixes

After fixing an error, always verify the fix yourself through the terminal before
reporting it as done. Don't rely on the user to confirm. For this project that means:

- Envs: `esp32c6-zigbee-bedroom`, `esp32c6-zigbee-office` (per-device temp
  offsets / LED defaults — see `platformio.ini`). Pick the one matching the
  connected board; the office board uses `TEMP_OFFSET_C=7.0f`.
- Build: `~/.platformio/penv/bin/pio run -e esp32c6-zigbee-office`
  (PlatformIO CLI is not on `PATH`; use the venv binary directly.)
- Upload: `~/.platformio/penv/bin/pio run -e esp32c6-zigbee-office -t upload`
  (close any `cat` / pyserial readers first — they hold the port exclusively.)
- Flash + monitor serial: read `/dev/cu.usbmodem*` at 115200 baud
  (use pyserial from `python3`, since `cat` on the tty drops bytes during
  ESP32-C6 boot panics). Capture ~20s to see a full boot cycle, then confirm
  the error is gone.

## Bumping firmware version

Single source of truth lives in two places that MUST be edited together:

1. `CMakeLists.txt`: `set(PROJECT_VER "X.Y.Z")` — drives `esp_app_desc_t::version`,
   shown as **Firmware ID** in Z2M (via Basic cluster `SW_BUILD_ID`).
2. `src/main.c`: `OTA_UPGRADE_FILE_VERSION` — uint32 encoded as `0xMMmmppbb`
   (major.minor.patch.build, 8 bits each). Read by Z2M as the device's
   **Firmware version** (OTA cluster `currentFileVersion`) and used to
   decide whether an `.ota` file is "newer".

Example: `1.0.1` → `0x01000100`, `1.2.3` → `0x01020300`.

After bumping:
- Build both envs: `~/.platformio/penv/bin/pio run -e esp32c6-zigbee-bedroom -e esp32c6-zigbee-office`
- Build the OTA image: `python scripts/build_ota.py` → `build/ota/scd41-xiao-0xMMmmppbb.ota`
  (the script parses `OTA_UPGRADE_FILE_VERSION` straight from `src/main.c`).
- Flash directly via USB (`-t upload`) or push via Z2M OTA (see below).

## sdkconfig layout

- `sdkconfig.defaults` — seed values for a fresh sdkconfig generation.
- `sdkconfig.esp32c6-zigbee-bedroom` / `sdkconfig.esp32c6-zigbee-office` —
  the actual persisted sdkconfigs used by the matching PlatformIO env.
  **Editing `sdkconfig.defaults` alone does not change behavior — each
  env-specific sdkconfig must be edited too (or deleted to regenerate).**
