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

## sdkconfig layout

- `sdkconfig.defaults` — seed values for a fresh sdkconfig generation.
- `sdkconfig.esp32c6-zigbee-bedroom` / `sdkconfig.esp32c6-zigbee-office` —
  the actual persisted sdkconfigs used by the matching PlatformIO env.
  **Editing `sdkconfig.defaults` alone does not change behavior — each
  env-specific sdkconfig must be edited too (or deleted to regenerate).**
