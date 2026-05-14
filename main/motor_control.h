#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MOTOR_L_PWM_PIN   4
#define MOTOR_L_DIR1_PIN  5
#define MOTOR_L_DIR2_PIN  6

#define MOTOR_R_PWM_PIN   7
#define MOTOR_R_DIR1_PIN  15
#define MOTOR_R_DIR2_PIN  16

#define MAX_SPEED         8191

typedef struct {
    int32_t left_target_cmd;
    int32_t right_target_cmd;
    float left_measured_pps;
    float right_measured_pps;
    uint32_t left_applied_duty;
    uint32_t right_applied_duty;
} motor_status_t;

typedef struct {
    float target_max_pps;
    float kp;
    float ki;
    float kd;
    float integral_limit;
    float correction_limit;
    uint32_t min_duty;
} motor_pid_config_t;

void motor_init(void);
void motor_move_forward(uint32_t speed);
void motor_move_backward(uint32_t speed);
void motor_turn_left(uint32_t speed);
void motor_turn_right(uint32_t speed);
void motor_stop(void);
void motor_set_left(int32_t speed);
void motor_set_right(int32_t speed);
void motor_set_differential(int32_t left_speed, int32_t right_speed);
void motor_get_status(motor_status_t *out_status);
void motor_get_pid_config(motor_pid_config_t *out_config);
esp_err_t motor_set_pid_config(const motor_pid_config_t *config);

#endif
