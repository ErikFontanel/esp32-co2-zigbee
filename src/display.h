#pragma once

#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

bool display_init(i2c_master_bus_handle_t bus);
void display_update(uint16_t co2_ppm, float temp_c, float rh_pct);
