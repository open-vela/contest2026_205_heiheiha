#include "care_audio.h"

#include <stdint.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CARE_AUDIO_BCLK_GPIO GPIO_NUM_41
#define CARE_AUDIO_WS_GPIO GPIO_NUM_42
#define CARE_AUDIO_DATA_GPIO GPIO_NUM_2
#define CARE_AUDIO_SAMPLE_RATE 48000
#define CARE_AUDIO_BUFFER_SAMPLES 256
#define CARE_AUDIO_FULL_SCALE 0x7fffffU
#define CARE_AUDIO_DISPLAY_GAIN 2U

static const char *TAG = "care_audio";
static i2s_chan_handle_t microphone_channel;
static SemaphoreHandle_t audio_lock;
static care_audio_status_t audio_status;
static TaskHandle_t audio_task_handle;

static uint8_t peak_level(const int32_t *samples, size_t count)
{
    uint32_t raw_peak = 0;
    for (size_t index = 0; index < count; ++index) {
        int64_t sample = samples[index];
        uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > raw_peak) raw_peak = magnitude;
    }
    uint32_t peak = raw_peak > 0x01000000U ? raw_peak >> 8 : raw_peak;
    uint64_t level = (uint64_t)peak * 100U * CARE_AUDIO_DISPLAY_GAIN / CARE_AUDIO_FULL_SCALE;
    return level > 100U ? 100U : (uint8_t)level;
}

static void audio_task(void *argument)
{
    (void)argument;
    int32_t samples[CARE_AUDIO_BUFFER_SAMPLES];
    while (true) {
        size_t bytes_read = 0;
        esp_err_t result = i2s_channel_read(microphone_channel, samples, sizeof(samples), &bytes_read, 1000);
        if (result != ESP_OK) {
            xSemaphoreTake(audio_lock, portMAX_DELAY);
            audio_status.errors++;
            uint32_t errors = audio_status.errors;
            xSemaphoreGive(audio_lock);
            if ((errors % 20U) == 1U) ESP_LOGW(TAG, "Microphone read failed: %s", esp_err_to_name(result));
            continue;
        }
        size_t count = bytes_read / sizeof(samples[0]);
        if (count == 0) continue;
        uint8_t level = peak_level(samples, count);
        xSemaphoreTake(audio_lock, portMAX_DELAY);
        audio_status.level = level;
        audio_status.samples += (uint32_t)count;
        xSemaphoreGive(audio_lock);
    }
}

esp_err_t care_audio_init(void)
{
    if (audio_task_handle) return ESP_OK;
    audio_lock = xSemaphoreCreateMutex();
    if (!audio_lock) return ESP_ERR_NO_MEM;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = CARE_AUDIO_BUFFER_SAMPLES;
    esp_err_t result = i2s_new_channel(&channel_config, NULL, &microphone_channel);
    if (result != ESP_OK) return result;

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CARE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CARE_AUDIO_BCLK_GPIO,
            .ws = CARE_AUDIO_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CARE_AUDIO_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    result = i2s_channel_init_std_mode(microphone_channel, &standard_config);
    if (result != ESP_OK) {
        i2s_del_channel(microphone_channel);
        microphone_channel = NULL;
        return result;
    }
    result = i2s_channel_enable(microphone_channel);
    if (result != ESP_OK) {
        i2s_del_channel(microphone_channel);
        microphone_channel = NULL;
        return result;
    }
    audio_status.ready = true;
    if (xTaskCreate(audio_task, "care_audio", 4096, NULL, 1, &audio_task_handle) != pdPASS) {
        i2s_channel_disable(microphone_channel);
        i2s_del_channel(microphone_channel);
        microphone_channel = NULL;
        audio_status.ready = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Microphone ready: I2S BCLK=%d WS=%d DIN=%d %d Hz",
        CARE_AUDIO_BCLK_GPIO, CARE_AUDIO_WS_GPIO, CARE_AUDIO_DATA_GPIO, CARE_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

void care_audio_get_status(care_audio_status_t *status)
{
    if (!status) return;
    if (!audio_lock) {
        *status = (care_audio_status_t){0};
        return;
    }
    xSemaphoreTake(audio_lock, portMAX_DELAY);
    *status = audio_status;
    xSemaphoreGive(audio_lock);
}
