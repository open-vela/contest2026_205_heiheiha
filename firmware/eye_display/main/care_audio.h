#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool ready;
    uint8_t level;
    uint32_t samples;
    uint32_t errors;
} care_audio_status_t;

esp_err_t care_audio_init(void);
void care_audio_get_status(care_audio_status_t *status);
