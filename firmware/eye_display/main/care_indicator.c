#include "care_indicator.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CARE_STATUS_LED GPIO_NUM_3

static volatile care_indicator_mode_t indicator_mode;
static volatile bool ble_active;
static TaskHandle_t indicator_task_handle;

static void led_write(bool on)
{
    gpio_set_level(CARE_STATUS_LED, on ? 0 : 1);
}

static void indicator_task(void *argument)
{
    (void)argument;
    uint32_t phase = 0;
    while (true) {
        care_indicator_mode_t mode = indicator_mode;
        uint32_t period = 0;
        bool on = false;
        if (mode == CARE_INDICATOR_MONITORING) {
            on = true;
        } else if (mode == CARE_INDICATOR_COUNTDOWN) {
            period = 300;
        } else if (mode == CARE_INDICATOR_ALARM) {
            period = 100;
        } else if (mode == CARE_INDICATOR_HOTSPOT || ble_active) {
            period = 1000;
        }
        if (period) {
            phase += 50;
            on = (phase % (period * 2)) < period;
        } else {
            phase = 0;
        }
        led_write(on);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t care_indicator_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CARE_STATUS_LED,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) return result;
    indicator_mode = CARE_INDICATOR_OFF;
    ble_active = false;
    led_write(false);
    if (xTaskCreate(indicator_task, "care_led", 2048, NULL, 1, &indicator_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void care_indicator_set_mode(care_indicator_mode_t mode)
{
    indicator_mode = mode;
}

void care_indicator_set_ble(bool active)
{
    ble_active = active;
}
