#include "fall_detector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FALL_BASELINE_SAMPLE_COUNT 12

static uint8_t sample_luma(const uint8_t *rgb888, uint16_t width, uint16_t height, int grid_x, int grid_y)
{
    uint16_t source_x = (uint32_t)grid_x * width / FALL_DETECTOR_GRID_WIDTH;
    uint16_t source_y = (uint32_t)grid_y * height / FALL_DETECTOR_GRID_HEIGHT;
    const uint8_t *pixel = rgb888 + ((size_t)source_y * width + source_x) * 3;
    return (uint8_t)(((uint16_t)pixel[0] * 77 + (uint16_t)pixel[1] * 150 + (uint16_t)pixel[2] * 29) >> 8);
}

esp_err_t fall_detector_init(fall_detector_t *detector)
{
    if (!detector) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(detector, 0, sizeof(*detector));
    detector->pose_threshold = 60;
    detector->recovery_threshold = 45;
    detector->motion_limit = 35;
    detector->hold_samples = 4;
    return ESP_OK;
}

void fall_detector_reset_tracking(fall_detector_t *detector)
{
    if (!detector) {
        return;
    }
    detector->previous_frame_valid = false;
    detector->sampled_frames = 0;
    detector->pending_inferences = 0;
    detector->motion_events = 0;
    detector->motion_score = 0;
    detector->posture_score = 0;
    detector->high_posture_samples = 0;
    detector->possible_fall = false;
    detector->baseline_ready = false;
    detector->baseline_samples = 0;
    memset(detector->baseline_luma_sum, 0, sizeof(detector->baseline_luma_sum));
}

esp_err_t fall_detector_load_model(fall_detector_t *detector, const uint8_t *model_data, size_t model_size)
{
    if (!detector || !model_data || model_size < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(model_data + 4, "TFL3", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    detector->model_data = model_data;
    detector->model_size = model_size;
    detector->model_loaded = true;
    return ESP_OK;
}

esp_err_t fall_detector_process_frame(fall_detector_t *detector, const uint8_t *rgb888, uint16_t width, uint16_t height)
{
    if (!detector || !rgb888 || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t difference_sum = 0;
    uint8_t current_luma[FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT];
    for (int grid_y = 0; grid_y < FALL_DETECTOR_GRID_HEIGHT; ++grid_y) {
        for (int grid_x = 0; grid_x < FALL_DETECTOR_GRID_WIDTH; ++grid_x) {
            int index = grid_y * FALL_DETECTOR_GRID_WIDTH + grid_x;
            current_luma[index] = sample_luma(rgb888, width, height, grid_x, grid_y);
            if (detector->previous_frame_valid) {
                difference_sum += (uint32_t)abs((int)current_luma[index] - detector->previous_luma[index]);
            }
        }
    }
    if (detector->previous_frame_valid) {
        uint32_t average_difference = difference_sum / (FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT);
        detector->motion_score = (uint8_t)(average_difference >= 64 ? 100 : average_difference * 100 / 64);
        if (!detector->model_loaded && detector->motion_score >= 55) {
            detector->motion_events++;
        }
    }
    if (!detector->baseline_ready) {
        for (int index = 0; index < FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT; ++index) {
            detector->baseline_luma_sum[index] += current_luma[index];
        }
        detector->baseline_samples++;
        if (detector->baseline_samples >= FALL_BASELINE_SAMPLE_COUNT) {
            for (int index = 0; index < FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT; ++index) {
                detector->baseline_luma[index] = (uint8_t)(detector->baseline_luma_sum[index] / FALL_BASELINE_SAMPLE_COUNT);
            }
            detector->baseline_ready = true;
        }
    } else {
        uint32_t posture_difference = 0;
        for (int index = 0; index < FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT; ++index) {
            posture_difference += (uint32_t)abs((int)current_luma[index] - detector->baseline_luma[index]);
        }
        uint32_t average_posture_difference = posture_difference /
            (FALL_DETECTOR_GRID_WIDTH * FALL_DETECTOR_GRID_HEIGHT);
        detector->posture_score = (uint8_t)(average_posture_difference >= 64 ? 100 : average_posture_difference * 100 / 64);
        if (detector->posture_score >= detector->pose_threshold && detector->motion_score <= detector->motion_limit) {
            if (detector->high_posture_samples < UINT8_MAX) {
                detector->high_posture_samples++;
            }
            if (detector->high_posture_samples >= detector->hold_samples) {
                detector->possible_fall = true;
            }
        } else {
            detector->high_posture_samples = 0;
        }
        if (detector->posture_score < detector->recovery_threshold) {
            detector->possible_fall = false;
        }
    }
    memcpy(detector->previous_luma, current_luma, sizeof(current_luma));
    detector->previous_frame_valid = true;
    detector->sampled_frames++;
    if (!detector->model_loaded) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    detector->pending_inferences++;
    return ESP_ERR_NOT_SUPPORTED;
}

void fall_detector_format_status(const fall_detector_t *detector, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }
    if (!detector) {
        snprintf(buffer, buffer_size, "AI: NOT READY\nDetector unavailable");
        return;
    }
    if (!detector->model_loaded) {
        if (!detector->baseline_ready) {
            snprintf(buffer, buffer_size, "AI: CALIBRATING\nBase: %u/%u\nSamples: %lu",
                detector->baseline_samples, FALL_BASELINE_SAMPLE_COUNT,
                (unsigned long)detector->sampled_frames);
        } else {
            if (detector->possible_fall) {
                snprintf(buffer, buffer_size, "AI: POSSIBLE FALL\nPose:%u%% Hold still\nSamples:%lu",
                    detector->posture_score, (unsigned long)detector->sampled_frames);
            } else {
                snprintf(buffer, buffer_size, "AI: DEMO FALL PROFILE\nMotion:%u%% Pose:%u%%\nSamples:%lu",
                    detector->motion_score, detector->posture_score,
                    (unsigned long)detector->sampled_frames);
            }
        }
        return;
    }
    snprintf(buffer, buffer_size, "AI: MODEL READY\nAdapter pending\nSamples: %lu",
        (unsigned long)detector->sampled_frames);
}
