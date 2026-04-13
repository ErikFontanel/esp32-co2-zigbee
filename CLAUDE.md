# Project notes

## Verifying fixes

After fixing an error, always verify the fix yourself through the terminal before
reporting it as done. Don't rely on the user to confirm. For this project that means:

- Build: `~/.platformio/penv/bin/pio run -e esp32c6-zigbee`
  (PlatformIO CLI is not on `PATH`; use the venv binary directly.)
- Upload: `~/.platformio/penv/bin/pio run -e esp32c6-zigbee -t upload`
  (close any `cat` / pyserial readers first — they hold the port exclusively.)
- Flash + monitor serial: read `/dev/cu.usbmodem*` at 115200 baud
  (use pyserial from `python3`, since `cat` on the tty drops bytes during
  ESP32-C6 boot panics). Capture ~20s to see a full boot cycle, then confirm
  the error is gone.

## sdkconfig layout

- `sdkconfig.defaults` — seed values for a fresh sdkconfig generation.
- `sdkconfig.esp32c6-zigbee` — the actual persisted sdkconfig used by the
  `esp32c6-zigbee` PlatformIO env. **Editing `sdkconfig.defaults` alone does
  not change behavior — the env-specific sdkconfig must be edited too (or
  deleted to regenerate).**
