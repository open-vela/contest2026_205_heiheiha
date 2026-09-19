#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    CARE_COMMAND_START,
    CARE_COMMAND_STOP,
    CARE_COMMAND_CANCEL,
    CARE_COMMAND_TEST,
    CARE_COMMAND_SHARE_ON,
    CARE_COMMAND_SHARE_OFF,
    CARE_COMMAND_REMINDER_ACK
} care_command_t;

typedef struct {
    bool camera_ready;
    bool camera_error;
    bool monitoring;
    bool baseline_ready;
    bool reminder_due;
    bool test_alert;
    uint8_t alert_state;
    uint8_t pose;
    uint8_t motion;
    uint16_t baseline_samples;
    uint32_t samples;
    uint32_t confirm_remaining;
    bool audio_ready;
    uint8_t audio_level;
    uint32_t audio_samples;
    uint32_t audio_errors;
    bool imu_ready;
    int16_t imu_x_mg;
    int16_t imu_y_mg;
    int16_t imu_z_mg;
    uint16_t imu_magnitude_mg;
    uint16_t imu_shock_mg;
    uint32_t imu_samples;
    uint32_t imu_errors;
    bool ble_ready;
} care_status_t;

esp_err_t care_portal_init(void);
bool care_portal_next_command(care_command_t *command);
esp_err_t care_portal_enqueue_command(care_command_t command);
void care_portal_get_status(care_status_t *status);
void care_portal_publish_status(const care_status_t *status);
void care_portal_publish_jpeg(const uint8_t *data, size_t size);
esp_err_t care_portal_set_sharing(bool enabled);
bool care_portal_sharing(void);
void care_portal_network_text(char *buffer, size_t size);
