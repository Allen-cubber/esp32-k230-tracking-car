#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "robot_track.h"

esp_err_t robot_mqtt_client_start(void);
bool robot_mqtt_client_read_track(robot_track_data_t *out_track);
esp_err_t robot_mqtt_client_send_gimbal_command(const char *action,
                                                int32_t pan_delta_deg,
                                                int32_t tilt_delta_deg,
                                                uint32_t duration_ms);
