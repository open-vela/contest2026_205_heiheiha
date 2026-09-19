#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    CARE_INDICATOR_OFF,
    CARE_INDICATOR_HOTSPOT,
    CARE_INDICATOR_MONITORING,
    CARE_INDICATOR_COUNTDOWN,
    CARE_INDICATOR_ALARM
} care_indicator_mode_t;

esp_err_t care_indicator_init(void);
void care_indicator_set_mode(care_indicator_mode_t mode);
void care_indicator_set_ble(bool active);
