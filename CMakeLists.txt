cmake_minimum_required(VERSION 3.16.0)

# Firmware version (semver). Becomes esp_app_desc_t::version, which is
# advertised over Zigbee Basic cluster SW_BUILD_ID (shown as "Firmware ID"
# in Z2M / Home Assistant). Keep in sync with OTA_UPGRADE_FILE_VERSION in
# src/main.c — encoded there as 0xMMmmppbb.
set(PROJECT_VER "1.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-co2)
