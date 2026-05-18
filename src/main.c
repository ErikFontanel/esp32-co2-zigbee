// ESP32-C6 + Sensirion SCD41 CO2 Sensor with Zigbee
// Seeed XIAO ESP32-C6 + SCD41 via I2C
// Exposes temperature, humidity, and CO2 via Zigbee HA profile

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "led_strip.h"
#include "display.h"

// ---------------------------------------------------------------
// Tags
// ---------------------------------------------------------------
static const char *TAG = "CO2_ZB";

// ---------------------------------------------------------------
// I2C / SCD41 Configuration
// ---------------------------------------------------------------
#define SCD41_SDA_PIN 4
#define SCD41_SCL_PIN 5
#define BUTTON_PIN    9
#define BUTTON_HOLD_MS 5000

// LED config. Default: WS2812 RGB on GPIO 8 (XIAO ESP32-C6 / Super Mini variants).
// Set LED_USE_WS2812 to 0 to fall back to a single-color GPIO LED; colors are
// then encoded as distinct blink patterns.
#ifndef LED_USE_WS2812
#define LED_USE_WS2812 1
#endif

#if LED_USE_WS2812
#define LED_PIN 8
#else
#define LED_PIN 15
#endif
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 100000
#define SCD41_ADDR 0x62
#define I2C_TIMEOUT_MS 100

// SCD41 commands
#define SCD41_START_PERIODIC 0x21B1
#define SCD41_STOP_PERIODIC 0x3F86
#define SCD41_READ_MEASUREMENT 0xEC05
#define SCD41_GET_DATA_READY 0xE4B8
#define SCD41_GET_SERIAL 0x3682
#define SCD41_SET_TEMP_OFFSET 0x241D

// ---------------------------------------------------------------
// Zigbee Configuration
// ---------------------------------------------------------------
#define ZB_ENDPOINT 1
#define CO2_CLUSTER_ID 0x040D
#define CO2_ATTR_MEASURED 0x0000
#define CO2_ATTR_MIN 0x0001
#define CO2_ATTR_MAX 0x0002

#define MEASURE_INTERVAL_MS 10000
#ifndef TEMP_OFFSET_C
#define TEMP_OFFSET_C       0.0f
#endif

// OTA configuration
#define OTA_UPGRADE_FILE_VERSION    0x00000001
#define OTA_UPGRADE_MANUFACTURER    0x1001
#define OTA_UPGRADE_IMAGE_TYPE      0x1011
#define OTA_UPGRADE_HW_VERSION      0x0001
#define OTA_UPGRADE_MAX_DATA_SIZE   223

// CO2 LED enable/disable default (overridable per-device via build flag)
#ifndef CO2_LED_DEFAULT
#define CO2_LED_DEFAULT 1
#endif

// ---------------------------------------------------------------
// Global sensor state
// ---------------------------------------------------------------
static volatile bool zb_connected = false;
static volatile uint16_t last_co2_ppm = 0;
static volatile bool co2_led_enabled = CO2_LED_DEFAULT;

// CO2 attribute storage (must be static for Zigbee references)
static float co2_val = 0.0f;
static float co2_min = 0.0f;
static float co2_max = 1.0f; // ZCL CO2 cluster uses fraction 0.0-1.0

// ---------------------------------------------------------------
// Sensirion CRC-8 (poly 0x31, init 0xFF)
// ---------------------------------------------------------------
static uint8_t scd41_crc(const uint8_t *data, size_t len)
{
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

// ---------------------------------------------------------------
// I2C (new bus/device API) + SCD41 helpers
// ---------------------------------------------------------------
static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t scd41_dev = NULL;

static esp_err_t i2c_init(void)
{
  if (i2c_bus)
    return ESP_OK;
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_PORT,
      .sda_io_num = SCD41_SDA_PIN,
      .scl_io_num = SCD41_SCL_PIN,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "I2C bus init failed");

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = SCD41_ADDR,
      .scl_speed_hz = I2C_FREQ_HZ,
  };
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &scd41_dev),
                      TAG, "I2C add device failed");
  return ESP_OK;
}

static esp_err_t scd41_send_cmd(uint16_t cmd)
{
  uint8_t buf[2] = {cmd >> 8, cmd & 0xFF};
  return i2c_master_transmit(scd41_dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t scd41_write_word(uint16_t cmd, uint16_t word)
{
  uint8_t data[2] = {word >> 8, word & 0xFF};
  uint8_t buf[5] = {cmd >> 8, cmd & 0xFF, data[0], data[1], scd41_crc(data, 2)};
  return i2c_master_transmit(scd41_dev, buf, 5, I2C_TIMEOUT_MS);
}

static esp_err_t scd41_read_words(uint16_t cmd, uint16_t *words, size_t count, uint32_t delay_ms)
{
  uint8_t cmd_buf[2] = {cmd >> 8, cmd & 0xFF};
  size_t rx_len = count * 3;
  uint8_t *rx = malloc(rx_len);
  if (!rx)
    return ESP_ERR_NO_MEM;

  esp_err_t ret;
  if (delay_ms > 0) {
    ret = i2c_master_transmit(scd41_dev, cmd_buf, 2, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) { free(rx); return ret; }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ret = i2c_master_receive(scd41_dev, rx, rx_len, I2C_TIMEOUT_MS);
  } else {
    ret = i2c_master_transmit_receive(scd41_dev, cmd_buf, 2, rx, rx_len, I2C_TIMEOUT_MS);
  }
  if (ret != ESP_OK) { free(rx); return ret; }

  for (size_t i = 0; i < count; i++)
  {
    uint8_t *w = rx + i * 3;
    if (scd41_crc(w, 2) != w[2])
    {
      ESP_LOGE(TAG, "CRC error word %d", (int)i);
      free(rx);
      return ESP_ERR_INVALID_CRC;
    }
    words[i] = (w[0] << 8) | w[1];
  }
  free(rx);
  return ESP_OK;
}

static esp_err_t scd41_init(void)
{
  ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C init failed");

  // Stop any running measurement
  esp_err_t ret = scd41_send_cmd(SCD41_STOP_PERIODIC);
  ESP_LOGI(TAG, "stop_periodic: %s", esp_err_to_name(ret));
  vTaskDelay(pdMS_TO_TICKS(500));

  // Read serial
  uint16_t sn[3];
  ret = scd41_read_words(SCD41_GET_SERIAL, sn, 3, 1);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "SCD41 serial: %04X%04X%04X", sn[0], sn[1], sn[2]);
  } else {
    ESP_LOGE(TAG, "get_serial: %s", esp_err_to_name(ret));
  }

  // Read current temperature offset, then set ours
  uint16_t cur_offset_word;
  ret = scd41_read_words(0x2318, &cur_offset_word, 1, 1);
  if (ret == ESP_OK) {
    float cur_offset = cur_offset_word * 175.0f / 65535.0f;
    ESP_LOGI(TAG, "SCD41 current temp_offset: %.2f C", (double)cur_offset);
  }

  if (TEMP_OFFSET_C != 0.0f) {
    uint16_t offset_word = (uint16_t)(TEMP_OFFSET_C * 65535.0f / 175.0f);
    ret = scd41_write_word(SCD41_SET_TEMP_OFFSET, offset_word);
    ESP_LOGI(TAG, "set_temp_offset(%.1f C): %s", (double)TEMP_OFFSET_C, esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  // Start periodic measurement
  ret = scd41_send_cmd(SCD41_START_PERIODIC);
  ESP_LOGI(TAG, "start_periodic: %s", esp_err_to_name(ret));
  ESP_RETURN_ON_ERROR(ret, TAG, "Start measurement failed");
  ESP_LOGI(TAG, "SCD41 periodic measurement started");
  vTaskDelay(pdMS_TO_TICKS(5000));
  return ESP_OK;
}

static esp_err_t scd41_read(uint16_t *co2_ppm, float *temp_c, float *rh_pct)
{
  // Check data ready
  uint16_t status;
  esp_err_t ret = scd41_read_words(SCD41_GET_DATA_READY, &status, 1, 1);
  if (ret != ESP_OK)
    return ret;
  if ((status & 0x07FF) == 0)
    return ESP_ERR_NOT_FINISHED;

  uint16_t words[3];
  ret = scd41_read_words(SCD41_READ_MEASUREMENT, words, 3, 1);
  if (ret != ESP_OK)
    return ret;

  *co2_ppm = words[0];
  *temp_c = -45.0f + 175.0f * (float)words[1] / 65535.0f;
  *rh_pct = 100.0f * (float)words[2] / 65535.0f;
  return ESP_OK;
}

// ---------------------------------------------------------------
// LED helpers — RGB state machine
//
// On a WS2812 we render colors directly. On a single-color GPIO LED we ignore
// the color and rely on the pattern (blink count + cadence) to disambiguate
// states. The state semantics:
//
//   OFF            steady off (idle, normal operation)
//   RESET_CONFIRM  three quick red blinks (boot button held past threshold)
//   PAIRING        rapid yellow flashing (network steering in progress / retry)
//   PAIRED_OK      three slow green blinks, then back to OFF
//   IDENTIFY       slow blue blinking while the coordinator's Identify is on
// ---------------------------------------------------------------
typedef enum {
  LED_STATE_OFF = 0,
  LED_STATE_RESET_CONFIRM,
  LED_STATE_PAIRING,
  LED_STATE_PAIRED_OK,
  LED_STATE_IDENTIFY,
} led_state_t;

static volatile led_state_t led_state = LED_STATE_OFF;

#if LED_USE_WS2812
static led_strip_handle_t led_strip = NULL;
#endif

static void led_init(void)
{
#if LED_USE_WS2812
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = LED_PIN,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000, // 10 MHz
      .flags.with_dma = false,
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led_strip));
  led_strip_clear(led_strip);
#else
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << LED_PIN,
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io);
  gpio_set_level(LED_PIN, 0);
#endif
}

// Render a single color. On WS2812: literal RGB. On a plain LED: any non-zero
// component lights it up; (0,0,0) turns it off.
static void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if LED_USE_WS2812
  if (!led_strip) return;
  led_strip_set_pixel(led_strip, 0, r, g, b);
  led_strip_refresh(led_strip);
#else
  gpio_set_level(LED_PIN, (r | g | b) ? 1 : 0);
#endif
}

// Sleep that aborts early if the LED state changes — keeps blink sequences
// from blocking a higher-priority state transition (e.g. PAIRING starting
// while a PAIRED_OK is still finishing).
static bool led_sleep(uint32_t ms, led_state_t expect)
{
  uint32_t step = 20;
  while (ms > 0) {
    uint32_t chunk = ms < step ? ms : step;
    vTaskDelay(pdMS_TO_TICKS(chunk));
    ms -= chunk;
    if (led_state != expect) return false;
  }
  return true;
}

static void led_blink_n(uint8_t r, uint8_t g, uint8_t b,
                        int count, uint32_t on_ms, uint32_t off_ms,
                        led_state_t expect)
{
  for (int i = 0; i < count; i++) {
    led_set_rgb(r, g, b);
    if (!led_sleep(on_ms, expect)) return;
    led_set_rgb(0, 0, 0);
    if (!led_sleep(off_ms, expect)) return;
  }
}

static void led_task(void *arg)
{
  while (1) {
    led_state_t s = led_state;
    switch (s) {
    case LED_STATE_OFF:
      if (co2_led_enabled && last_co2_ppm >= 800) {
        if (last_co2_ppm >= 5000)
          led_set_rgb(255, 0, 0);
        else if (last_co2_ppm >= 2000)
          led_set_rgb(255, 60, 0);
        else
          led_set_rgb(255, 180, 0);
      } else {
        led_set_rgb(0, 0, 0);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      break;

    case LED_STATE_RESET_CONFIRM:
      led_blink_n(255, 0, 0, 3, 150, 150, s);
      // Hand off to whatever state was scheduled next (pairing kicks in after
      // the device reboots into factory-new). If nothing else changed, idle.
      if (led_state == s) led_state = LED_STATE_OFF;
      break;

    case LED_STATE_PAIRING:
      // Rapid yellow flashing — keeps going until state changes.
      led_set_rgb(255, 90, 0);
      if (!led_sleep(80, s)) break;
      led_set_rgb(0, 0, 0);
      led_sleep(80, s);
      break;

    case LED_STATE_PAIRED_OK:
#if LED_USE_WS2812
      // Three green "breaths" — ramp brightness up then down.
      for (int i = 0; i < 3 && led_state == s; i++) {
        const int steps = 32;
        const uint32_t step_ms = 25; // 32 * 25 * 2 = 1.6s per breath
        for (int k = 0; k <= steps && led_state == s; k++) {
          uint8_t g = (uint8_t)((k * 255) / steps);
          led_set_rgb(0, g, 0);
          if (!led_sleep(step_ms, s)) break;
        }
        for (int k = steps; k >= 0 && led_state == s; k--) {
          uint8_t g = (uint8_t)((k * 255) / steps);
          led_set_rgb(0, g, 0);
          if (!led_sleep(step_ms, s)) break;
        }
      }
#else
      // Single-LED fallback: still binary, just slow blinks.
      led_blink_n(0, 255, 0, 3, 600, 400, s);
#endif
      if (led_state == s) led_state = LED_STATE_OFF;
      break;

    case LED_STATE_IDENTIFY:
      led_set_rgb(255, 255, 255);
      if (!led_sleep(250, s)) break;
      led_set_rgb(0, 0, 0);
      led_sleep(250, s);
      break;
    }
  }
}

// ---------------------------------------------------------------
// Button task: hold BUTTON_HOLD_MS for factory reset
// ---------------------------------------------------------------
static void button_task(void *arg)
{
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io);

  uint32_t held_ms = 0;
  while (1) {
    if (gpio_get_level(BUTTON_PIN) == 0) {
      held_ms += 100;
      if (held_ms >= BUTTON_HOLD_MS) {
        ESP_LOGW(TAG, "Button held %u ms -> factory reset", (unsigned)held_ms);
        // Play 3x red blink confirmation, then trigger reset (which reboots).
        led_state = LED_STATE_RESET_CONFIRM;
        vTaskDelay(pdMS_TO_TICKS(3 * (150 + 150) + 100));
        esp_zb_factory_reset();
      }
    } else {
      held_ms = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------------------------------------------------------
// Wrapper for scheduler alarm (correct callback signature)
// ---------------------------------------------------------------
static void bdb_start_top_level_commissioning_cb(uint8_t mode)
{
  esp_zb_bdb_start_top_level_commissioning(mode);
}

// ---------------------------------------------------------------
// Zigbee signal handler
// ---------------------------------------------------------------
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
  uint32_t *p_sg_p = signal_struct->p_app_signal;
  esp_err_t err = signal_struct->esp_err_status;
  esp_zb_app_signal_type_t sig = *p_sg_p;

  switch (sig)
  {
  case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
    ESP_LOGI(TAG, "Zigbee stack initialized");
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
    break;

  case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
  case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
    if (err == ESP_OK)
    {
      ESP_LOGI(TAG, "Device started (%s)",
               esp_zb_bdb_is_factory_new() ? "factory new" : "provisioned");
      if (esp_zb_bdb_is_factory_new())
      {
        led_state = LED_STATE_PAIRING;
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
      }
      else
      {
        // Already paired — silent boot.
        led_state = LED_STATE_OFF;
        zb_connected = true;
      }
    }
    else
    {
      ESP_LOGW(TAG, "Zigbee BDB init failed: %s, retrying steering...", esp_err_to_name(err));
      led_state = LED_STATE_PAIRING;
      esp_zb_scheduler_alarm(
          bdb_start_top_level_commissioning_cb,
          ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
    }
    break;

  case ESP_ZB_BDB_SIGNAL_STEERING:
    if (err == ESP_OK)
    {
      ESP_LOGI(TAG, "Joined network (ch:%d addr:0x%04hx)",
               esp_zb_get_current_channel(), esp_zb_get_short_address());
      zb_connected = true;
      led_state = LED_STATE_PAIRED_OK;
    }
    else
    {
      ESP_LOGW(TAG, "Zigbee steering failed: %s, retry in 1s", esp_err_to_name(err));
      led_state = LED_STATE_PAIRING;
      esp_zb_scheduler_alarm(
          bdb_start_top_level_commissioning_cb,
          ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
    }
    break;

  default:
    ESP_LOGI(TAG, "ZDO signal: 0x%x status: %s", sig, esp_err_to_name(err));
    break;
  }
}

// ---------------------------------------------------------------
// OTA upgrade status handler
// ---------------------------------------------------------------
static esp_err_t zb_ota_upgrade_status_handler(esp_zb_zcl_ota_upgrade_value_message_t message)
{
  static uint32_t total_size = 0;
  static uint32_t offset = 0;

  if (message.info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    switch (message.upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
      ESP_LOGI(TAG, "OTA upgrade start");
      total_size = message.ota_header.image_size;
      offset = 0;
      break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
      offset += message.payload_size;
      ESP_LOGI(TAG, "OTA progress: %lu/%lu bytes (%lu%%)",
               offset, total_size, total_size ? (offset * 100) / total_size : 0);
      break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
      ESP_LOGI(TAG, "OTA applying upgrade");
      break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
      return (offset == total_size) ? ESP_OK : ESP_FAIL;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
      ESP_LOGI(TAG, "OTA finished, rebooting");
      esp_restart();
      break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
      ESP_LOGW(TAG, "OTA aborted");
      break;
    default:
      break;
    }
  }
  return ESP_OK;
}

// ---------------------------------------------------------------
// Zigbee action handler
// ---------------------------------------------------------------
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
  switch (callback_id) {
  case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
    return zb_ota_upgrade_status_handler(*(esp_zb_zcl_ota_upgrade_value_message_t *)message);
  case ESP_ZB_CORE_IDENTIFY_EFFECT_CB_ID: {
    esp_zb_zcl_identify_effect_message_t *m = (esp_zb_zcl_identify_effect_message_t *)message;
    ESP_LOGI(TAG, "Identify effect id=0x%x", m->effect_id);
    bool active = (m->effect_id != ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_FINISH_EFFECT &&
                   m->effect_id != ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_STOP);
    led_state = active ? LED_STATE_IDENTIFY : LED_STATE_OFF;
    return ESP_OK;
  }
  case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
    esp_zb_zcl_set_attr_value_message_t *m = (esp_zb_zcl_set_attr_value_message_t *)message;
    if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY &&
        m->attribute.id == ESP_ZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID) {
      uint16_t t = *(uint16_t *)m->attribute.data.value;
      ESP_LOGI(TAG, "IdentifyTime=%u", t);
      led_state = (t > 0) ? LED_STATE_IDENTIFY : LED_STATE_OFF;
    }
    if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
      co2_led_enabled = *(bool *)m->attribute.data.value;
      ESP_LOGI(TAG, "CO2 LED %s", co2_led_enabled ? "enabled" : "disabled");
    }
    return ESP_OK;
  }
  default:
    ESP_LOGI(TAG, "Action callback: 0x%x", callback_id);
    break;
  }
  return ESP_OK;
}

// ---------------------------------------------------------------
// Update Zigbee attributes from sensor readings
// ---------------------------------------------------------------
static void zb_update_attributes(uint16_t co2_ppm, float temp_c, float rh_pct)
{
  int16_t zb_temp = (int16_t)(temp_c * 100);
  uint16_t zb_hum = (uint16_t)(rh_pct * 100);
  float zb_co2 = (float)co2_ppm / 1000000.0f; // ppm to fraction

  esp_zb_lock_acquire(portMAX_DELAY);

  // `check=false`: MeasuredValue attributes are READ_ONLY|REPORTING in the ZCL
  // spec, so check=true would fail the write. The stack auto-reports to bound
  // destinations (Z2M's external converter binds + configureReporting on join).
  esp_zb_zcl_set_attribute_val(ZB_ENDPOINT,
                               ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                               &zb_temp, false);

  esp_zb_zcl_set_attribute_val(ZB_ENDPOINT,
                               ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                               &zb_hum, false);

  esp_zb_zcl_set_attribute_val(ZB_ENDPOINT,
                               ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID,
                               &zb_co2, false);

  esp_zb_lock_release();
}

// ---------------------------------------------------------------
// Sensor reading task
// ---------------------------------------------------------------
static void sensor_task(void *arg)
{
  while (scd41_init() != ESP_OK)
  {
    ESP_LOGW(TAG, "SCD41 init failed, retrying in 5s...");
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  display_init(i2c_bus);

  while (1)
  {
    uint16_t co2_ppm;
    float temp_c, rh_pct;

    if (scd41_read(&co2_ppm, &temp_c, &rh_pct) == ESP_OK)
    {
      ESP_LOGI(TAG, "CO2: %u ppm  T: %.1f C  RH: %.1f %%",
               co2_ppm, temp_c, rh_pct);

      last_co2_ppm = co2_ppm;
      display_update(co2_ppm, temp_c, rh_pct);

      if (zb_connected)
      {
        zb_update_attributes(co2_ppm, temp_c, rh_pct);
      }
    }
    else
    {
      ESP_LOGW(TAG, "Sensor read failed or not ready");
    }

    vTaskDelay(pdMS_TO_TICKS(MEASURE_INTERVAL_MS));
  }
}

// ---------------------------------------------------------------
// Zigbee task
// ---------------------------------------------------------------
static void esp_zb_task(void *arg)
{
  esp_zb_cfg_t zb_cfg = {
      .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
      .install_code_policy = false,
      .nwk_cfg.zczr_cfg = {
          .max_children = 10,
      },
  };
  esp_zb_init(&zb_cfg);

  // --- Basic cluster ---
  esp_zb_basic_cluster_cfg_t basic_cfg = {
      .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
      .power_source = 0x01,
  };
  esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, "\x09Sensirion");
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, "\x0aSCD41-XIAO");

  // Firmware / build info for HA and Z2M
  const esp_app_desc_t *app = esp_app_get_description();
  static char sw_build[1 + 17];
  size_t blen = strnlen(app->version, 16);
  sw_build[0] = (char)blen;
  memcpy(sw_build + 1, app->version, blen);
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw_build);

  static char date_code[1 + 17];
  size_t dlen = strnlen(app->date, 16);
  date_code[0] = (char)dlen;
  memcpy(date_code + 1, app->date, dlen);
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);

  static uint8_t app_version = 1;
  static uint8_t stack_version = 1;
  static uint8_t hw_version = OTA_UPGRADE_HW_VERSION;
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
  esp_zb_basic_cluster_add_attr(basic_cluster,
                                ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);

  // --- Identify cluster ---
  esp_zb_identify_cluster_cfg_t identify_cfg = {.identify_time = 0};
  esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);

  // --- On/Off cluster (controls CO2 LED indicator) ---
  esp_zb_on_off_cluster_cfg_t onoff_cfg = {.on_off = co2_led_enabled};
  esp_zb_attribute_list_t *onoff_cluster = esp_zb_on_off_cluster_create(&onoff_cfg);

  // --- Temperature cluster ---
  esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
      .measured_value = 2200, // 22.00 C initial
      .min_value = -4000,
      .max_value = 8500,
  };
  esp_zb_attribute_list_t *temp_cluster = esp_zb_temperature_meas_cluster_create(&temp_cfg);

  // --- Humidity cluster ---
  esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
      .measured_value = 5000, // 50.00 % initial
      .min_value = 0,
      .max_value = 10000,
  };
  esp_zb_attribute_list_t *hum_cluster = esp_zb_humidity_meas_cluster_create(&hum_cfg);

  // --- CO2 cluster (native 0x040D) ---
  esp_zb_carbon_dioxide_measurement_cluster_cfg_t co2_cfg = {
      .measured_value = 0.0f,
      .min_measured_value = 0.0f,
      .max_measured_value = 1.0f,
  };
  esp_zb_attribute_list_t *co2_cluster = esp_zb_carbon_dioxide_measurement_cluster_create(&co2_cfg);

  // --- OTA cluster (client role) ---
  esp_zb_ota_cluster_cfg_t ota_cfg = {
      .ota_upgrade_file_version = OTA_UPGRADE_FILE_VERSION,
      .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
      .ota_upgrade_manufacturer = OTA_UPGRADE_MANUFACTURER,
      .ota_upgrade_image_type = OTA_UPGRADE_IMAGE_TYPE,
  };
  esp_zb_attribute_list_t *ota_cluster = esp_zb_ota_cluster_create(&ota_cfg);

  esp_zb_zcl_ota_upgrade_client_variable_t ota_client_var = {
      .timer_query = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,
      .hw_version = OTA_UPGRADE_HW_VERSION,
      .max_data_size = OTA_UPGRADE_MAX_DATA_SIZE,
  };
  uint16_t ota_server_addr = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_ADDR_DEF_VALUE;
  uint8_t ota_server_ep = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_ENDPOINT_DEF_VALUE;

  esp_zb_ota_cluster_add_attr(ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, (void *)&ota_client_var);
  esp_zb_ota_cluster_add_attr(ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, (void *)&ota_server_addr);
  esp_zb_ota_cluster_add_attr(ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, (void *)&ota_server_ep);

  // --- Build cluster list ---
  esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_on_off_cluster(cluster_list, onoff_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, hum_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_carbon_dioxide_measurement_cluster(cluster_list, co2_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_ota_cluster(cluster_list, ota_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

  // --- Build endpoint ---
  esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
  esp_zb_endpoint_config_t ep_cfg = {
      .endpoint = ZB_ENDPOINT,
      .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
      .app_device_version = 0,
  };
  esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);

  // --- Register device ---
  esp_zb_device_register(ep_list);
  esp_zb_core_action_handler_register(zb_action_handler);

  // Default reporting config for temp, humidity, CO2
  esp_zb_zcl_reporting_info_t temp_report = {
      .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
      .ep = ZB_ENDPOINT,
      .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
      .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
      .attr_id = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
      .u.send_info.min_interval = 30,
      .u.send_info.max_interval = 300,
      .u.send_info.def_min_interval = 30,
      .u.send_info.def_max_interval = 300,
      .u.send_info.delta.s16 = 20, // 0.20 C
      .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
  };
  esp_zb_zcl_update_reporting_info(&temp_report);

  esp_zb_zcl_reporting_info_t hum_report = temp_report;
  hum_report.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
  hum_report.attr_id = ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID;
  hum_report.u.send_info.delta.u16 = 100; // 1.00 %
  esp_zb_zcl_update_reporting_info(&hum_report);

  esp_zb_zcl_reporting_info_t co2_report = temp_report;
  co2_report.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT;
  co2_report.attr_id = ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID;
  co2_report.u.send_info.delta.s16 = 0; // time-based only (float attr)
  esp_zb_zcl_update_reporting_info(&co2_report);

  esp_zb_set_rx_on_when_idle(true);

  // 2.4 GHz Zigbee channels 11-26
  esp_zb_set_primary_network_channel_set(0x07FFF800);
  esp_zb_set_secondary_network_channel_set(0x07FFF800);
  ESP_ERROR_CHECK(esp_zb_start(false));

  // Per SDK header, must be called after esp_zb_start()
  esp_zb_set_node_descriptor_power_source(true); // mains-powered

  esp_zb_stack_main_loop();
}

// ---------------------------------------------------------------
// app_main
// ---------------------------------------------------------------
void app_main(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);


  esp_zb_platform_config_t platform_cfg = {
      .radio_config = {
          .radio_mode = ZB_RADIO_MODE_NATIVE,
      },
      .host_config = {
          .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
      },
  };
  ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

  led_init();
  xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
  xTaskCreate(button_task, "button", 2048, NULL, 3, NULL);
  xTaskCreate(sensor_task, "sensor", 4096, NULL, 4, NULL);
  xTaskCreate(esp_zb_task, "zigbee", 4096, NULL, 5, NULL);
}
