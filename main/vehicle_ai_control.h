#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t vehicle_ai_control_init(void);
esp_err_t vehicle_ai_control_submit_text(const char *text);
void vehicle_ai_control_update(void);
bool vehicle_ai_control_is_manual_mode(void);
