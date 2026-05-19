#pragma once

#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

bool display_init(i2c_master_bus_handle_t bus);
bool display_is_present(void);
void display_update(uint16_t co2_ppm, float temp_c, float rh_pct);

// Turn the panel on or off (SSD1306 0xAF / 0xAE). When transitioning to on,
// the current framebuffer is pushed so the user sees the latest readings.
void display_set_power(bool on);

// Set OLED contrast (SSD1306 0x81). The SSD1306 has no true PWM brightness;
// contrast adjusts drive current and is the standard way to "dim" the panel.
void display_set_brightness(uint8_t level);
