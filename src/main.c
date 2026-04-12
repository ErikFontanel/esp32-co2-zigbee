// ESP32-C6 + Sensirion SCD41 CO2 Sensor with Zigbee
// Seeed XIAO ESP32-C6 + SCD41 via I2C
// Exposes temperature, humidity, and CO2 via Zigbee HA profile

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"

// ---------------------------------------------------------------
// Tags
// ---------------------------------------------------------------
static const char *TAG = "CO2_ZB";

// ---------------------------------------------------------------
// I2C / SCD41 Configuration
// ---------------------------------------------------------------
#define SCD41_SDA_PIN 4
#define SCD41_SCL_PIN 5
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 100000
#define SCD41_ADDR 0x62
#define I2C_TIMEOUT_TICKS pdMS_TO_TICKS(100)

// SCD41 commands
#define SCD41_START_PERIODIC 0x21B1
#define SCD41_STOP_PERIODIC 0x3F86
#define SCD41_READ_MEASUREMENT 0xEC05
#define SCD41_GET_DATA_READY 0xE4B8
#define SCD41_GET_SERIAL 0x3682

// ---------------------------------------------------------------
// Zigbee Configuration
// ---------------------------------------------------------------
#define ZB_ENDPOINT 1
#define CO2_CLUSTER_ID 0x040D
#define CO2_ATTR_MEASURED 0x0000
#define CO2_ATTR_MIN 0x0001
#define CO2_ATTR_MAX 0x0002

#define MEASURE_INTERVAL_MS 10000

// ---------------------------------------------------------------
// Global sensor state
// ---------------------------------------------------------------
static volatile bool zb_connected = false;

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
// I2C helpers
// ---------------------------------------------------------------
static esp_err_t scd41_send_cmd(uint16_t cmd)
{
  uint8_t buf[2] = {cmd >> 8, cmd & 0xFF};
  i2c_cmd_handle_t h = i2c_cmd_link_create();
  i2c_master_start(h);
  i2c_master_write_byte(h, (SCD41_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write(h, buf, 2, true);
  i2c_master_stop(h);
  esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, h, I2C_TIMEOUT_TICKS);
  i2c_cmd_link_delete(h);
  return ret;
}

static esp_err_t scd41_read_words(uint16_t cmd, uint16_t *words, size_t count, uint32_t delay_ms)
{
  esp_err_t ret = scd41_send_cmd(cmd);
  if (ret != ESP_OK)
    return ret;

  vTaskDelay(pdMS_TO_TICKS(delay_ms));

  size_t rx_len = count * 3;
  uint8_t *rx = malloc(rx_len);
  if (!rx)
    return ESP_ERR_NO_MEM;

  i2c_cmd_handle_t h = i2c_cmd_link_create();
  i2c_master_start(h);
  i2c_master_write_byte(h, (SCD41_ADDR << 1) | I2C_MASTER_READ, true);
  i2c_master_read(h, rx, rx_len, I2C_MASTER_LAST_NACK);
  i2c_master_stop(h);
  ret = i2c_master_cmd_begin(I2C_PORT, h, I2C_TIMEOUT_TICKS);
  i2c_cmd_link_delete(h);

  if (ret != ESP_OK)
  {
    free(rx);
    return ret;
  }

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

// ---------------------------------------------------------------
// I2C + SCD41 init
// ---------------------------------------------------------------
static bool i2c_initialized = false;

static esp_err_t i2c_init(void)
{
  if (i2c_initialized)
    return ESP_OK;
  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = SCD41_SDA_PIN,
      .scl_io_num = SCD41_SCL_PIN,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = I2C_FREQ_HZ,
  };
  ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &conf), TAG, "I2C config failed");
  ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "I2C install failed");
  i2c_initialized = true;
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
  *temp_c = -45.0f + 175.0f * (float)words[1] / 65535.0f - 1.0f;
  *rh_pct = 100.0f * (float)words[2] / 65535.0f;
  return ESP_OK;
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
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
      }
      else
      {
        zb_connected = true;
      }
    }
    else
    {
      ESP_LOGW(TAG, "Init failed: %s, retrying...", esp_err_to_name(err));
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
    }
    else
    {
      ESP_LOGW(TAG, "Steering failed, retry in 1s");
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
// Zigbee action handler
// ---------------------------------------------------------------
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
  ESP_LOGI(TAG, "Action callback: 0x%x", callback_id);
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

  while (1)
  {
    uint16_t co2_ppm;
    float temp_c, rh_pct;

    if (scd41_read(&co2_ppm, &temp_c, &rh_pct) == ESP_OK)
    {
      ESP_LOGI(TAG, "CO2: %u ppm  T: %.1f C  RH: %.1f %%",
               co2_ppm, temp_c, rh_pct);

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
      .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
      .install_code_policy = false,
      .nwk_cfg.zed_cfg = {
          .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
          .keep_alive = 3000,
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

  // --- Identify cluster ---
  esp_zb_identify_cluster_cfg_t identify_cfg = {.identify_time = 0};
  esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);

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

  // --- Build cluster list ---
  esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, hum_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_carbon_dioxide_measurement_cluster(cluster_list, co2_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

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

  esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
  ESP_ERROR_CHECK(esp_zb_start(false));
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

  xTaskCreate(sensor_task, "sensor", 4096, NULL, 4, NULL);
  xTaskCreate(esp_zb_task, "zigbee", 4096, NULL, 5, NULL);
}
