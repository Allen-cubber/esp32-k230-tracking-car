#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

esp_err_t vehicle_ai_control_init(void);
esp_err_t vehicle_ai_control_submit_text(const char *text);
esp_err_t vehicle_ai_control_execute_action_payload(const cJSON *payload,
                                                    char *error_buf,
                                                    size_t error_buf_size);
void vehicle_ai_control_update(void);
bool vehicle_ai_control_is_manual_mode(void);
