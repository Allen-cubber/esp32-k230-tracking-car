#include "motor_control.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motor";

#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL_L  LEDC_CHANNEL_0
#define LEDC_CHANNEL_R  LEDC_CHANNEL_1
#define LEDC_DUTY_RES   LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY  5000

// Closed-loop speed control tuning. The upper layer still sends the old PWM-like
// speed command range [-8191, 8191]; this layer maps it to encoder pulses per second
// and adds bounded PID correction on top of the feed-forward duty.
#define MOTOR_CLOSED_LOOP_PERIOD_MS       50
#define MOTOR_ENCODER_TARGET_MAX_PPS      3000.0f
#define MOTOR_SPEED_PID_KP                0.65f
#define MOTOR_SPEED_PID_KI                0.18f
#define MOTOR_SPEED_PID_KD                0.0f
#define MOTOR_SPEED_PID_INTEGRAL_LIMIT    4000.0f
#define MOTOR_SPEED_PID_CORRECTION_LIMIT  2200.0f
#define MOTOR_CLOSED_LOOP_MIN_DUTY        500

typedef struct {
    int32_t target_cmd;
    float measured_pps;
    float integral;
    float prev_error;
    uint32_t applied_duty;
} motor_loop_t;

static motor_loop_t s_left_loop;
static motor_loop_t s_right_loop;
static portMUX_TYPE s_motor_lock = portMUX_INITIALIZER_UNLOCKED;

static float clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int32_t clamp_speed_signed(int32_t speed)
{
    if (speed > MAX_SPEED) {
        return MAX_SPEED;
    }
    if (speed < -MAX_SPEED) {
        return -MAX_SPEED;
    }
    return speed;
}

static uint32_t clamp_speed_abs(int32_t speed)
{
    speed = clamp_speed_signed(speed);
    if (speed < 0) {
        speed = -speed;
    }
    return (uint32_t)speed;
}

static int32_t clamp_speed_u32(uint32_t speed)
{
    if (speed > MAX_SPEED) {
        return MAX_SPEED;
    }
    return (int32_t)speed;
}

static void apply_duty(ledc_channel_t channel, uint32_t duty)
{
    if (duty > MAX_SPEED) {
        duty = MAX_SPEED;
    }
    ledc_set_duty(LEDC_MODE, channel, duty);
    ledc_update_duty(LEDC_MODE, channel);
}

static void set_left_direction(bool forward)
{
    gpio_set_level(MOTOR_L_DIR1_PIN, forward ? 1 : 0);
    gpio_set_level(MOTOR_L_DIR2_PIN, forward ? 0 : 1);
}

static void set_right_direction(bool forward)
{
    gpio_set_level(MOTOR_R_DIR1_PIN, forward ? 1 : 0);
    gpio_set_level(MOTOR_R_DIR2_PIN, forward ? 0 : 1);
}

static void release_left_motor(void)
{
    gpio_set_level(MOTOR_L_DIR1_PIN, 0);
    gpio_set_level(MOTOR_L_DIR2_PIN, 0);
    apply_duty(LEDC_CHANNEL_L, 0);
}

static void release_right_motor(void)
{
    gpio_set_level(MOTOR_R_DIR1_PIN, 0);
    gpio_set_level(MOTOR_R_DIR2_PIN, 0);
    apply_duty(LEDC_CHANNEL_R, 0);
}

static void motor_loop_reset(motor_loop_t *loop)
{
    loop->integral = 0.0f;
    loop->prev_error = 0.0f;
    loop->measured_pps = 0.0f;
    loop->applied_duty = 0;
}

static uint32_t motor_loop_compute_duty(motor_loop_t *loop, int32_t target_cmd,
                                        float measured_pps, float dt_s)
{
    if (target_cmd == 0) {
        motor_loop_reset(loop);
        return 0;
    }

    const float target_pps = ((float)abs(target_cmd) / (float)MAX_SPEED) *
                             MOTOR_ENCODER_TARGET_MAX_PPS;
    const float error = target_pps - measured_pps;

    loop->integral += error * dt_s;
    loop->integral = clamp_float(loop->integral,
                                 -MOTOR_SPEED_PID_INTEGRAL_LIMIT,
                                 MOTOR_SPEED_PID_INTEGRAL_LIMIT);

    const float derivative = dt_s > 0.0f ? (error - loop->prev_error) / dt_s : 0.0f;
    loop->prev_error = error;
    loop->measured_pps = measured_pps;

    float correction = MOTOR_SPEED_PID_KP * error +
                       MOTOR_SPEED_PID_KI * loop->integral +
                       MOTOR_SPEED_PID_KD * derivative;
    correction = clamp_float(correction,
                             -MOTOR_SPEED_PID_CORRECTION_LIMIT,
                             MOTOR_SPEED_PID_CORRECTION_LIMIT);

    float duty = (float)abs(target_cmd) + correction;
    if (duty > 0.0f && duty < MOTOR_CLOSED_LOOP_MIN_DUTY) {
        duty = MOTOR_CLOSED_LOOP_MIN_DUTY;
    }
    duty = clamp_float(duty, 0.0f, (float)MAX_SPEED);

    loop->applied_duty = (uint32_t)duty;
    return loop->applied_duty;
}

static void motor_control_task(void *arg)
{
    (void)arg;

    const TickType_t period_ticks = pdMS_TO_TICKS(MOTOR_CLOSED_LOOP_PERIOD_MS);
    const float dt_s = (float)MOTOR_CLOSED_LOOP_PERIOD_MS / 1000.0f;

    while (true) {
        vTaskDelay(period_ticks);

        int32_t left_delta = 0;
        int32_t right_delta = 0;
        if (encoder_read_and_clear(&left_delta, &right_delta) != ESP_OK) {
            continue;
        }

        const float left_pps = ((float)abs(left_delta) * 1000.0f) /
                               (float)MOTOR_CLOSED_LOOP_PERIOD_MS;
        const float right_pps = ((float)abs(right_delta) * 1000.0f) /
                                (float)MOTOR_CLOSED_LOOP_PERIOD_MS;

        int32_t left_target;
        int32_t right_target;
        portENTER_CRITICAL(&s_motor_lock);
        left_target = s_left_loop.target_cmd;
        right_target = s_right_loop.target_cmd;
        portEXIT_CRITICAL(&s_motor_lock);

        const uint32_t left_duty = motor_loop_compute_duty(&s_left_loop, left_target, left_pps, dt_s);
        const uint32_t right_duty = motor_loop_compute_duty(&s_right_loop, right_target, right_pps, dt_s);

        if (left_target != 0) {
            apply_duty(LEDC_CHANNEL_L, left_duty);
        }
        if (right_target != 0) {
            apply_duty(LEDC_CHANNEL_R, right_duty);
        }
    }
}

static void motor_set_target(motor_loop_t *loop, int32_t speed)
{
    portENTER_CRITICAL(&s_motor_lock);
    const bool old_active = loop->target_cmd != 0;
    const bool new_active = speed != 0;
    const bool direction_changed = old_active && new_active &&
                                   ((loop->target_cmd > 0) != (speed > 0));
    if (!old_active || !new_active || direction_changed) {
        loop->integral = 0.0f;
        loop->prev_error = 0.0f;
    }
    loop->target_cmd = speed;
    portEXIT_CRITICAL(&s_motor_lock);
}

static void motor_start_closed_loop_if_ready(void)
{
    esp_err_t ret = encoder_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "encoder init failed (%s), motor stays open-loop", esp_err_to_name(ret));
        return;
    }

    if (xTaskCreate(motor_control_task, "motor_ctrl", 4096, NULL, 8, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to start motor control task, motor stays open-loop");
        return;
    }

    ESP_LOGI(TAG,
             "speed closed-loop enabled period=%dms target_max=%.0fpps kp=%.2f ki=%.2f kd=%.2f",
             MOTOR_CLOSED_LOOP_PERIOD_MS,
             MOTOR_ENCODER_TARGET_MAX_PPS,
             MOTOR_SPEED_PID_KP,
             MOTOR_SPEED_PID_KI,
             MOTOR_SPEED_PID_KD);
}

void motor_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MOTOR_L_DIR1_PIN) |
                        (1ULL << MOTOR_L_DIR2_PIN) |
                        (1ULL << MOTOR_R_DIR1_PIN) |
                        (1ULL << MOTOR_R_DIR2_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    gpio_set_level(MOTOR_L_DIR1_PIN, 0);
    gpio_set_level(MOTOR_L_DIR2_PIN, 0);
    gpio_set_level(MOTOR_R_DIR1_PIN, 0);
    gpio_set_level(MOTOR_R_DIR2_PIN, 0);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel_l = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_L,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = MOTOR_L_PWM_PIN,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel_l);

    ledc_channel_config_t ledc_channel_r = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_R,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = MOTOR_R_PWM_PIN,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel_r);

    apply_duty(LEDC_CHANNEL_L, 0);
    apply_duty(LEDC_CHANNEL_R, 0);

    motor_start_closed_loop_if_ready();
}

void motor_move_forward(uint32_t speed)
{
    const int32_t cmd = clamp_speed_u32(speed);
    motor_set_differential(cmd, cmd);
}

void motor_move_backward(uint32_t speed)
{
    const int32_t cmd = clamp_speed_u32(speed);
    motor_set_differential(-cmd, -cmd);
}

void motor_turn_left(uint32_t speed)
{
    const int32_t cmd = clamp_speed_u32(speed);
    motor_set_differential(-cmd, cmd);
}

void motor_turn_right(uint32_t speed)
{
    const int32_t cmd = clamp_speed_u32(speed);
    motor_set_differential(cmd, -cmd);
}

void motor_stop(void)
{
    motor_set_target(&s_left_loop, 0);
    motor_set_target(&s_right_loop, 0);
    motor_loop_reset(&s_left_loop);
    motor_loop_reset(&s_right_loop);
    release_left_motor();
    release_right_motor();
}

void motor_set_left(int32_t speed)
{
    speed = clamp_speed_signed(speed);
    motor_set_target(&s_left_loop, speed);

    if (speed == 0) {
        motor_loop_reset(&s_left_loop);
        release_left_motor();
        return;
    }

    set_left_direction(speed > 0);
    apply_duty(LEDC_CHANNEL_L, clamp_speed_abs(speed));
}

void motor_set_right(int32_t speed)
{
    speed = clamp_speed_signed(speed);
    motor_set_target(&s_right_loop, speed);

    if (speed == 0) {
        motor_loop_reset(&s_right_loop);
        release_right_motor();
        return;
    }

    set_right_direction(speed > 0);
    apply_duty(LEDC_CHANNEL_R, clamp_speed_abs(speed));
}

void motor_set_differential(int32_t left_speed, int32_t right_speed)
{
    motor_set_left(left_speed);
    motor_set_right(right_speed);
}
