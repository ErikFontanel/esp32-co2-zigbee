# ESP32-C6 CO2 Zigbee Sensor

A Zigbee CO2, temperature, and humidity sensor using an ESP32-C6 and Sensirion SCD41. Pairs with Zigbee2MQTT and Home Assistant.

## Hardware

- **MCU**: ESP32-C6 Super Mini (any ESP32-C6 board with native 802.15.4 radio)
- **Sensor**: Sensirion SCD41 (I2C)

### Wiring

| SCD41 | ESP32-C6 Super Mini |
|-------|-------------------|
| SDA   | GPIO4             |
| SCL   | GPIO5             |
| VDD   | 3.3V              |
| GND   | GND               |

> Adjust `SCD41_SDA_PIN` and `SCD41_SCL_PIN` in `src/main.c` if using a different board or pinout.
>
> Adjust `TEMP_OFFSET_C` via the PlatformIO `build_flags` in `platformio.ini` to calibrate the temperature reading against a reference sensor (default: `-1.0`).

### Optional: OLED Display

An SSD1306 128×32 I2C OLED can be connected to the same I2C bus (address `0x3C`). It is runtime-detected — if absent, the firmware runs normally without it. When present, it shows CO2, temperature, and humidity readings.

## Zigbee Clusters

| Cluster | ID     | Data            |
|---------|--------|-----------------|
| Temperature Measurement | 0x0402 | int16, 0.01 C units |
| Relative Humidity | 0x0405 | uint16, 0.01% units |
| Carbon Dioxide Measurement | 0x040D | float, fraction (ppm / 1,000,000) |
| On/Off | 0x0006 | bool — controls the CO2 level indicator LED |

## Building & Flashing

Requires [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Flash
pio run -t upload

# Serial monitor
pio device monitor
```

The Zigbee libraries (`esp-zigbee-lib`, `esp-zboss-lib`) are pulled automatically from the Espressif component registry via `src/idf_component.yml`.

## Zigbee2MQTT Setup

The device will show temperature and humidity out of the box. For CO2 support, you need an external converter.

1. Copy `external_converters/scd41.mjs` to your Zigbee2MQTT config directory:
   ```
   /config/zigbee2mqtt/external_converters/scd41.mjs
   ```
2. Restart Zigbee2MQTT
3. Put your Zigbee coordinator in pairing mode
4. The device will join automatically on first boot (or after a factory reset)

The device shows up as `SCD41-XIAO` with CO2 (ppm), temperature (C), and humidity (%) exposes.

## Factory Reset

Hold the BOOT button (GPIO 9) for 5 seconds. The device confirms the reset, reboots, and re-pairs automatically.

Alternatively, erase the flash and re-upload:

```bash
pio run -t erase
pio run -t upload
```

## LED Status

Drives a WS2812 on GPIO 8 (set `LED_USE_WS2812=0` in `src/main.c` to fall back to a single-color LED on GPIO 15 with equivalent blink patterns).

| State | LED |
|---|---|
| Idle, CO2 < 800 ppm | off |
| Idle, CO2 800–1999 ppm | steady yellow |
| Idle, CO2 2000–4999 ppm | steady orange |
| Idle, CO2 ≥ 5000 ppm | steady red |
| Reset confirmed | 3× red flash |
| Pairing | rapid yellow flashing |
| Paired | 3× green breathing, then off |
| Identify (from coordinator) | slow white blink |

The CO2 indicator can be toggled on/off via Zigbee (On/Off cluster), which shows up as a "CO2 LED" switch in Zigbee2MQTT. Set the default per device with the `CO2_LED_DEFAULT` build flag in `platformio.ini` (1 = on, 0 = off).

## OTA Updates

The device has a Zigbee OTA client. To push a new firmware image:

1. Bump `OTA_UPGRADE_FILE_VERSION` in `src/main.c`.
2. Build firmware and OTA image:
   ```bash
   pio run -e esp32c6-zigbee-office
   python scripts/build_ota.py
   ```
3. Upload the resulting `build/ota/scd41-xiao-*.ota` directly through the device's OTA panel in the Zigbee2MQTT web UI.

The device verifies the image, applies it, and reboots automatically. Progress is logged over serial.

## Project Structure

```
src/main.c                  # Firmware: I2C sensor + Zigbee stack
src/idf_component.yml       # ESP-IDF Zigbee component dependencies
external_converters/scd41.mjs  # Zigbee2MQTT external converter
zigbee.csv                  # Partition table (includes zb_storage)
sdkconfig.defaults          # Zigbee + flash config
platformio.ini              # PlatformIO build config
```

## Development

Firmware lives in C (`src/`) and is built with PlatformIO — see [Building & Flashing](#building--flashing).

Non-firmware files (the Z2M converter, helper scripts) are linted and formatted with [Biome](https://biomejs.dev/). On first clone:

```bash
npm install            # installs Biome
npm run install-hooks  # points git at .githooks/ for the pre-commit check
```

After that, every commit runs `biome check` on staged JS/JSON files. Manual commands:

```bash
npm run lint       # check formatting + lint
npm run lint:fix   # auto-fix where possible
npm run format     # format only
```

Bypass once with `git commit --no-verify`.

## Disclaimer

This project was built using intuitive engineering (some people call it "vibe coding") with [Claude Code](https://claude.ai/code). The firmware works, the sensor reads, Zigbee reports -- but this has not been rigorously tested, reviewed for edge cases, or validated for production use. Use at your own risk. If your smart home burns down because of a rogue CO2 reading, that's on you.

No warranty. No guarantees. Just vibes.

## License

MIT
