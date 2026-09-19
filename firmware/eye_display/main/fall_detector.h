#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FALL_DETECTOR_GRID_WIDTH 16
#define FALL_DETECTOR_GRID_HEIGHT 12

typedef struct {
    const uint8_t *model_data;
    size_t model_size;
    bool model_loaded;
    bool previous_frame_valid;
    bool baseline_ready;
    uint32_t sampled_frames;
    uint32_t pending_inferences;
    uint32_t motion_events;
    uint16_t baseline_samples;
    uint8_t high_posture_samples;
    uint8_t motion_score;
    uint8_t posture_score;
    bool possible_fall;
    uint8_t pose_threshold;
    uint8_t recovery_threshold;
    uint8_t motion_limit;
    uint8_t hold_samples;
    uint8_t previous_luma[FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT];
    uint8_t baseline_luma[FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT];
    uint32_t baseline_luma_sum[FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT];
} fall_detector_t;

esp_err_t fall_detector_init(fall_detector_t *detector);
esp_err_t fall_detector_load_model(fall_detector_t *detector, const uint8_t *model_data, size_t model_size);
esp_err_t fall_detector_process_frame(fall_detector_t *detector, const uint8_t *rgb888, uint16_t width, uint16_t height);
void fall_detector_reset_tracking(fall_detector_t *detector);
void fall_detector_format_status(const fall_detector_t *detector, char *buffer, size_t buffer_size);
