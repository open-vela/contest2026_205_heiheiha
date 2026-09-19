#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CARE_HISTORY_CAPACITY 32
#define CARE_CONFIG_VERSION 1

typedef struct {
    uint32_t version;
    uint8_t pose_threshold;
    uint8_t recovery_threshold;
    uint8_t motion_limit;
    uint8_t hold_samples;
    uint16_t confirm_seconds;
    uint16_t reminder_minutes;
    char ssid[33];
    char wifi_password[65];
    char device_key[13];
} care_config_t;

typedef enum {
    CARE_EVENT_BOOT,
    CARE_EVENT_MONITOR_START,
    CARE_EVENT_MONITOR_STOP,
    CARE_EVENT_CANDIDATE,
    CARE_EVENT_ALARM,
    CARE_EVENT_CANCEL,
    CARE_EVENT_TEST_START,
    CARE_EVENT_TEST_ALARM,
    CARE_EVENT_TEST_CANCEL,
    CARE_EVENT_REMINDER,
    CARE_EVENT_REMINDER_ACK,
    CARE_EVENT_WIFI_CONNECTED,
    CARE_EVENT_WIFI_LOST,
    CARE_EVENT_SETTINGS,
    CARE_EVENT_COUNT
} care_event_kind_t;

typedef struct {
    uint32_t id;
    uint32_t boot_id;
    uint64_t uptime_seconds;
    int64_t unix_seconds;
    uint8_t kind;
    uint8_t pose;
    uint8_t motion;
    uint8_t reserved;
} care_event_t;

esp_err_t care_store_init(void);
void care_config_defaults(care_config_t *config);
bool care_config_valid(const care_config_t *config);
void care_store_get_config(care_config_t *config);
esp_err_t care_store_save_config(const care_config_t *config);
void care_store_record(care_event_kind_t kind, uint8_t pose, uint8_t motion);
size_t care_store_history(care_event_t *events, size_t capacity);
esp_err_t care_store_clear_history(void);
uint32_t care_store_error_count(void);
const char *care_event_name(care_event_kind_t kind);
