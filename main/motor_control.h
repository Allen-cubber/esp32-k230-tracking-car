#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#define MOTOR_L_PWM_PIN   4
#define MOTOR_L_DIR1_PIN  5
#define MOTOR_L_DIR2_PIN  6

#define MOTOR_R_PWM_PIN   7
#define MOTOR_R_DIR1_PIN  15
#define MOTOR_R_DIR2_PIN  16

#define MAX_SPEED         8191

void motor_init(void);
void motor_move_forward(uint32_t speed);
void motor_move_backward(uint32_t speed);
void motor_turn_left(uint32_t speed);
void motor_turn_right(uint32_t speed);
void motor_stop(void);
void motor_set_left(int32_t speed);
void motor_set_right(int32_t speed);
void motor_set_differential(int32_t left_speed, int32_t right_speed);

#endif
