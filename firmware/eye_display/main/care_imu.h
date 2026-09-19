#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool ready;
    uint8_t address;
    uint8_t chip_id;
    int16_t x_mg;
    int16_t y_mg;
    int16_t z_mg;
    uint16_t magnitude_mg;
    uint16_t shock_mg;
    uint32_t samples;
    uint32_t errors;
} care_imu_status_t;

esp_err_t care_imu_init(void);
void care_imu_get_status(care_imu_status_t *status);
