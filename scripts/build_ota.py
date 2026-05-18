#!/usr/bin/env python3
"""Build a Zigbee OTA image from the PlatformIO firmware binary.

Reads OTA_UPGRADE_* defines from src/main.c so the OTA header always
matches the running firmware. Output goes to build/ota/<name>.ota.

Usage:
    python scripts/build_ota.py <env>
    # e.g. python scripts/build_ota.py esp32c6-zigbee-bedroom
"""

import re
import struct
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
MAIN_C = PROJECT_DIR / "src/main.c"
OUTPUT_DIR = PROJECT_DIR / "build/ota"

# Zigbee OTA file header constants (ZCL spec)
OTA_FILE_ID = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
OTA_HEADER_MIN_LENGTH = 56  # without optional fields
OTA_TAG_UPGRADE_IMAGE = 0x0000


def parse_defines(path: Path) -> dict[str, int]:
    """Extract OTA_UPGRADE_* #defines from main.c."""
    text = path.read_text()
    defines = {}
    for m in re.finditer(r"#define\s+(OTA_UPGRADE_\w+)\s+(0x[\da-fA-F]+|\d+)", text):
        defines[m.group(1)] = int(m.group(2), 0)
    return defines


def build_ota_header(defines: dict[str, int], image_size: int) -> bytes:
    """Build a Zigbee OTA file header with the upgrade image tag."""
    manufacturer = defines["OTA_UPGRADE_MANUFACTURER"]
    image_type = defines["OTA_UPGRADE_IMAGE_TYPE"]
    file_version = defines["OTA_UPGRADE_FILE_VERSION"]
    hw_version = defines["OTA_UPGRADE_HW_VERSION"]

    # field_control bit 2 = hardware version present
    field_control = 0x0004
    header_length = OTA_HEADER_MIN_LENGTH + 4  # +4 for hw min/max (2 x uint16)

    # Sub-element: tag header (tag_id u16 + length u32) + image payload
    tag_header = struct.pack("<HI", OTA_TAG_UPGRADE_IMAGE, image_size)
    total_size = header_length + len(tag_header) + image_size

    parts = []
    parts.append(struct.pack("<I", OTA_FILE_ID))
    parts.append(struct.pack("<H", OTA_HEADER_VERSION))
    parts.append(struct.pack("<H", header_length))
    parts.append(struct.pack("<H", field_control))
    parts.append(struct.pack("<H", manufacturer))
    parts.append(struct.pack("<H", image_type))
    parts.append(struct.pack("<I", file_version))
    parts.append(struct.pack("<H", 0x0000))  # zigbee stack version
    parts.append(struct.pack("<32s", b"SCD41-XIAO-CO2"))  # header string, zero-padded
    parts.append(struct.pack("<I", total_size))
    # Optional: hardware version range (field_control bit 2)
    parts.append(struct.pack("<H", hw_version))  # min hw version
    parts.append(struct.pack("<H", hw_version))  # max hw version

    header = b"".join(parts)
    assert len(header) == header_length, f"Header length mismatch: {len(header)} != {header_length}"

    return header + tag_header


def main():
    if len(sys.argv) != 2:
        print("Usage: python scripts/build_ota.py <env>")
        print("  e.g. python scripts/build_ota.py esp32c6-zigbee-bedroom")
        sys.exit(1)

    env = sys.argv[1]
    firmware_bin = PROJECT_DIR / f".pio/build/{env}/firmware.bin"
    if not firmware_bin.exists():
        print(f"Error: firmware not found at {firmware_bin}")
        print(f"Run '~/.platformio/penv/bin/pio run -e {env}' first.")
        sys.exit(1)

    defines = parse_defines(MAIN_C)
    for key in ("OTA_UPGRADE_FILE_VERSION", "OTA_UPGRADE_MANUFACTURER",
                "OTA_UPGRADE_IMAGE_TYPE", "OTA_UPGRADE_HW_VERSION"):
        if key not in defines:
            print(f"Error: {key} not found in {MAIN_C}")
            sys.exit(1)

    firmware = firmware_bin.read_bytes()
    header = build_ota_header(defines, len(firmware))

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    version = defines["OTA_UPGRADE_FILE_VERSION"]
    output_name = f"scd41-xiao-{env}-{version:#010x}.ota"
    output_path = OUTPUT_DIR / output_name

    output_path.write_bytes(header + firmware)

    print(f"OTA image: {output_path}")
    print(f"  Env:          {env}")
    print(f"  Manufacturer: {defines['OTA_UPGRADE_MANUFACTURER']:#06x}")
    print(f"  Image type:   {defines['OTA_UPGRADE_IMAGE_TYPE']:#06x}")
    print(f"  File version: {version:#010x}")
    print(f"  Firmware:     {len(firmware)} bytes")
    print(f"  Total:        {len(header) + len(firmware)} bytes")


if __name__ == "__main__":
    main()
