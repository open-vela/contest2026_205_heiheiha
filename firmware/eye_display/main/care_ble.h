#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t care_ble_init(void);
bool care_ble_ready(void);
