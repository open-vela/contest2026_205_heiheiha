#include "care_imu.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_SCCB_HARDWARE_I2C_PORT1
#define CARE_IMU_I2C_PORT I2C_NUM_1
#else
#define CARE_IMU_I2C_PORT I2C_NUM_0
#endif
#define QMA6100P_ADDRESS_0 0x12
#define QMA6100P_ADDRESS_1 0x13
#define QMA6100P_CHIP_ID_REG 0x00
#define QMA6100P_XOUT_L_REG 0x01

static const char *TAG = "care_imu";
static SemaphoreHandle_t imu_lock;
static TaskHandle_t imu_task_handle;
static i2c_master_bus_handle_t imu_bus;
static i2c_master_dev_handle_t imu_device;
static care_imu_status_t imu_status;
static uint8_t imu_address;

static esp_err_t imu_read(uint8_t reg, uint8_t *data, size_t length)
{
    if (!imu_device || !data || !length) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(imu_device, &reg, 1, data, length, 100);
}

static void imu_task(void *argument)
{
    (void)argument;
    uint16_t previous_magnitude = 1000;
    while (true) {
        uint8_t sample[6];
        esp_err_t result = imu_read(QMA6100P_XOUT_L_REG, sample, sizeof(sample));
        if (result != ESP_OK) {
            xSemaphoreTake(imu_lock, portMAX_DELAY);
            imu_status.errors++;
            xSemaphoreGive(imu_lock);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        int16_t raw_x = (int16_t)(((uint16_t)sample[1] << 8) | sample[0]) >> 2;
        int16_t raw_y = (int16_t)(((uint16_t)sample[3] << 8) | sample[2]) >> 2;
        int16_t raw_z = (int16_t)(((uint16_t)sample[5] << 8) | sample[4]) >> 2;
        int32_t x_mg = (int32_t)raw_x * 244 / 1000;
        int32_t y_mg = (int32_t)raw_y * 244 / 1000;
        int32_t z_mg = (int32_t)raw_z * 244 / 1000;
        int64_t square_sum = (int64_t)x_mg * x_mg + (int64_t)y_mg * y_mg + (int64_t)z_mg * z_mg;
        uint32_t magnitude = (uint32_t)(sqrtf((float)square_sum) + 0.5f);
        if (magnitude > UINT16_MAX) magnitude = UINT16_MAX;
        uint32_t delta = magnitude > previous_magnitude ? magnitude - previous_magnitude : previous_magnitude - magnitude;
        if (delta > UINT16_MAX) delta = UINT16_MAX;
        previous_magnitude = (uint16_t)magnitude;
        xSemaphoreTake(imu_lock, portMAX_DELAY);
        imu_status.x_mg = (int16_t)x_mg;
        imu_status.y_mg = (int16_t)y_mg;
        imu_status.z_mg = (int16_t)z_mg;
        imu_status.magnitude_mg = (uint16_t)magnitude;
        imu_status.shock_mg = (uint16_t)((imu_status.shock_mg * 3 + delta) / 4);
        imu_status.samples++;
        xSemaphoreGive(imu_lock);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t care_imu_init(void)
{
    if (imu_task_handle) return ESP_OK;
    if (!imu_lock) {
        imu_lock = xSemaphoreCreateMutex();
        if (!imu_lock) return ESP_ERR_NO_MEM;
    }
    esp_err_t result = i2c_master_get_bus_handle(CARE_IMU_I2C_PORT, &imu_bus);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Camera I2C bus is not ready: %s", esp_err_to_name(result));
        return result;
    }
    uint8_t addresses[] = {QMA6100P_ADDRESS_0, QMA6100P_ADDRESS_1};
    uint8_t chip_id = 0;
    result = ESP_ERR_NOT_FOUND;
    for (size_t index = 0; index < sizeof(addresses); ++index) {
        if (i2c_master_probe(imu_bus, addresses[index], 100) != ESP_OK) continue;
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[index],
            .scl_speed_hz = 100000,
        };
        result = i2c_master_bus_add_device(imu_bus, &device_config, &imu_device);
        if (result != ESP_OK) continue;
        result = imu_read(QMA6100P_CHIP_ID_REG, &chip_id, 1);
        if (result == ESP_OK) {
            imu_address = addresses[index];
            break;
        }
        i2c_master_bus_rm_device(imu_device);
        imu_device = NULL;
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "QMA6100P not found on GPIO4/GPIO5");
        return result;
    }
    if ((chip_id & 0xF0) != 0x90) {
        ESP_LOGW(TAG, "Unexpected QMA6100P chip id: 0x%02X", chip_id);
    }
    xSemaphoreTake(imu_lock, portMAX_DELAY);
    memset(&imu_status, 0, sizeof(imu_status));
    imu_status.ready = true;
    imu_status.address = imu_address;
    imu_status.chip_id = chip_id;
    xSemaphoreGive(imu_lock);
    if (xTaskCreate(imu_task, "care_imu", 3072, NULL, 1, &imu_task_handle) != pdPASS) {
        xSemaphoreTake(imu_lock, portMAX_DELAY);
        imu_status.ready = false;
        xSemaphoreGive(imu_lock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "QMA6100P ready: addr=0x%02X id=0x%02X", imu_address, chip_id);
    return ESP_OK;
}

void care_imu_get_status(care_imu_status_t *status)
{
    if (!status) return;
    if (!imu_lock) {
        memset(status, 0, sizeof(*status));
        return;
    }
    xSemaphoreTake(imu_lock, portMAX_DELAY);
    *status = imu_status;
    xSemaphoreGive(imu_lock);
}
