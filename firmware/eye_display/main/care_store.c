#include "care_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CARE_HISTORY_VERSION 1

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t next_id;
    care_event_t entries[CARE_HISTORY_CAPACITY];
} care_history_t;

static nvs_handle_t storage;
static SemaphoreHandle_t storage_lock;
static QueueHandle_t event_queue;
static care_config_t current_config;
static care_history_t history;
static uint32_t boot_id;
static uint32_t storage_errors;
static portMUX_TYPE error_lock = portMUX_INITIALIZER_UNLOCKED;

static void record_error(void)
{
    portENTER_CRITICAL(&error_lock);
    storage_errors++;
    portEXIT_CRITICAL(&error_lock);
}

void care_config_defaults(care_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->version = CARE_CONFIG_VERSION;
    config->pose_threshold = 60;
    config->recovery_threshold = 45;
    config->motion_limit = 35;
    config->hold_samples = 4;
    config->confirm_seconds = 5;
}

bool care_config_valid(const care_config_t *config)
{
    return config && config->version == CARE_CONFIG_VERSION &&
        config->pose_threshold >= 10 && config->pose_threshold <= 100 &&
        config->recovery_threshold >= 1 && config->recovery_threshold < config->pose_threshold &&
        config->motion_limit <= 100 && config->hold_samples >= 2 && config->hold_samples <= 20 &&
        config->confirm_seconds >= 3 && config->confirm_seconds <= 60 &&
        (config->reminder_minutes == 0 || (config->reminder_minutes >= 5 && config->reminder_minutes <= 1440)) &&
        memchr(config->ssid, 0, sizeof(config->ssid)) &&
        memchr(config->wifi_password, 0, sizeof(config->wifi_password)) &&
        config->device_key[12] == 0 && strspn(config->device_key, "0123456789ABCDEF") == 12 &&
        (!config->ssid[0] || (strlen(config->wifi_password) >= 8 && strlen(config->wifi_password) <= 63));
}

static void event_writer(void *argument)
{
    (void)argument;
    care_event_t event;
    while (xQueueReceive(event_queue, &event, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(storage_lock, portMAX_DELAY);
        event.id = history.next_id++;
        if (history.count == CARE_HISTORY_CAPACITY) {
            memmove(history.entries, history.entries + 1,
                (CARE_HISTORY_CAPACITY - 1) * sizeof(history.entries[0]));
            history.count--;
        }
        history.entries[history.count++] = event;
        esp_err_t result = nvs_set_blob(storage, "history", &history, sizeof(history));
        if (result == ESP_OK) result = nvs_commit(storage);
        xSemaphoreGive(storage_lock);
        if (result != ESP_OK) {
            record_error();
            ESP_LOGE("care_store", "History persistence failed: %s", esp_err_to_name(result));
        }
    }
}

esp_err_t care_store_init(void)
{
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result;
    result = nvs_open("care", NVS_READWRITE, &storage);
    if (result != ESP_OK) return result;
    storage_lock = xSemaphoreCreateMutex();
    event_queue = xQueueCreate(16, sizeof(care_event_t));
    if (!storage_lock || !event_queue) return ESP_ERR_NO_MEM;
    size_t length = sizeof(current_config);
    result = nvs_get_blob(storage, "config", &current_config, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        care_config_defaults(&current_config);
        snprintf(current_config.device_key, sizeof(current_config.device_key), "%08lX%04lX",
            (unsigned long)esp_random(), (unsigned long)(esp_random() & 0xffff));
        result = nvs_set_blob(storage, "config", &current_config, sizeof(current_config));
    } else if (result == ESP_OK && (length != sizeof(current_config) || !care_config_valid(&current_config))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK) return result;
    length = sizeof(history);
    result = nvs_get_blob(storage, "history", &history, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        memset(&history, 0, sizeof(history));
        history.version = CARE_HISTORY_VERSION;
        history.next_id = 1;
    } else if (result != ESP_OK || length != sizeof(history) ||
        history.version != CARE_HISTORY_VERSION || history.count > CARE_HISTORY_CAPACITY) {
        return ESP_ERR_INVALID_STATE;
    }
    result = nvs_get_u32(storage, "boot_id", &boot_id);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return result;
    boot_id++;
    result = nvs_set_u32(storage, "boot_id", boot_id);
    if (result == ESP_OK) result = nvs_commit(storage);
    if (result != ESP_OK) return result;
    if (xTaskCreate(event_writer, "care_store", 4096, NULL, 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI("care_store", "Storage ready: boot=%lu, retained events=%lu",
        (unsigned long)boot_id, (unsigned long)history.count);
    care_store_record(CARE_EVENT_BOOT, 0, 0);
    return ESP_OK;
}

void care_store_get_config(care_config_t *config)
{
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    *config = current_config;
    xSemaphoreGive(storage_lock);
}

esp_err_t care_store_save_config(const care_config_t *config)
{
    if (!care_config_valid(config)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    esp_err_t result = nvs_set_blob(storage, "config", config, sizeof(*config));
    if (result == ESP_OK) result = nvs_commit(storage);
    if (result == ESP_OK) current_config = *config;
    xSemaphoreGive(storage_lock);
    if (result != ESP_OK) record_error();
    return result;
}

void care_store_record(care_event_kind_t kind, uint8_t pose, uint8_t motion)
{
    if (!event_queue || kind >= CARE_EVENT_COUNT) return;
    time_t now = time(NULL);
    care_event_t event = {
        .boot_id = boot_id,
        .uptime_seconds = (uint64_t)esp_timer_get_time() / 1000000,
        .unix_seconds = now >= 1704067200 ? (int64_t)now : 0,
        .kind = kind,
        .pose = pose,
        .motion = motion,
    };
    if (xQueueSend(event_queue, &event, 0) != pdTRUE) record_error();
}

size_t care_store_history(care_event_t *events, size_t capacity)
{
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    size_t count = history.count < capacity ? history.count : capacity;
    for (size_t index = 0; index < count; ++index) events[index] = history.entries[history.count - 1 - index];
    xSemaphoreGive(storage_lock);
    return count;
}

esp_err_t care_store_clear_history(void)
{
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    uint32_t previous_count = history.count;
    history.count = 0;
    esp_err_t result = nvs_set_blob(storage, "history", &history, sizeof(history));
    if (result == ESP_OK) result = nvs_commit(storage);
    if (result != ESP_OK) history.count = previous_count;
    xSemaphoreGive(storage_lock);
    if (result != ESP_OK) record_error();
    return result;
}

uint32_t care_store_error_count(void)
{
    portENTER_CRITICAL(&error_lock);
    uint32_t count = storage_errors;
    portEXIT_CRITICAL(&error_lock);
    return count;
}

const char *care_event_name(care_event_kind_t kind)
{
    static const char *names[] = {
        "boot", "monitor_start", "monitor_stop", "candidate", "alarm", "cancel",
        "test_start", "test_alarm", "test_cancel", "reminder", "reminder_ack",
        "wifi_connected", "wifi_lost", "settings"
    };
    return kind < CARE_EVENT_COUNT ? names[kind] : "unknown";
}
