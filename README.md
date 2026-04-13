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
> Adjust `TEMP_OFFSET_C` in `src/main.c` to calibrate the temperature reading against a reference sensor (default: `-1.0`).

## Zigbee Clusters

| Cluster | ID     | Data            |
|---------|--------|-----------------|
| Temperature Measurement | 0x0402 | int16, 0.01 C units |
| Relative Humidity | 0x0405 | uint16, 0.01% units |
| Carbon Dioxide Measurement | 0x040D | float, fraction (ppm / 1,000,000) |

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
| Idle / paired | off |
| Reset confirmed | 3× red flash |
| Pairing | rapid yellow flashing |
| Paired | 3× green breathing, then off |
| Identify (from coordinator) | slow white blink |

## OTA Updates

The device has a Zigbee OTA client. To push a new firmware image:

1. Bump `OTA_UPGRADE_FILE_VERSION` in `src/main.c`.
2. Build firmware and OTA image:
   ```bash
   pio run -e esp32c6-zigbee
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

## Disclaimer

This project was built using intuitive engineering (some people call it "vibe coding") with [Claude Code](https://claude.ai/code). The firmware works, the sensor reads, Zigbee reports -- but this has not been rigorously tested, reviewed for edge cases, or validated for production use. Use at your own risk. If your smart home burns down because of a rogue CO2 reading, that's on you.

No warranty. No guarantees. Just vibes.

## License

MIT
