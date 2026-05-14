#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "motor_control.h"
#include "robot_track.h"

typedef struct {
    uint32_t uptime_ms;
    const char *state;
    bool manual_mode;
    bool obstacle_hold;

    float ultrasonic_cm;

    bool pose_ready;
    float yaw_deg;
    float yaw_rate_dps;

    bool track_seen;
    robot_track_data_t track;

    int32_t left_target_cmd;
    int32_t right_target_cmd;
    float left_measured_pps;
    float right_measured_pps;
    uint32_t left_applied_duty;
    uint32_t right_applied_duty;

    int32_t left_encoder_count;
    int32_t right_encoder_count;

    float face_size;
    float target_yaw_deg;
    float heading_error_deg;
    float turn_output;
    int32_t target_speed_cmd;
    int32_t base_speed_cmd;

    motor_pid_config_t motor_pid;
} robot_status_telemetry_t;

esp_err_t robot_mqtt_client_start(void);
bool robot_mqtt_client_read_track(robot_track_data_t *out_track);
esp_err_t robot_mqtt_client_send_gimbal_command(const char *action,
                                                int32_t pan_delta_deg,
                                                int32_t tilt_delta_deg,
                                                uint32_t duration_ms);
esp_err_t robot_mqtt_client_publish_status(const robot_status_telemetry_t *status);
