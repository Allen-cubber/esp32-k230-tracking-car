#include "motor_control.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL_L  LEDC_CHANNEL_0
#define LEDC_CHANNEL_R  LEDC_CHANNEL_1
#define LEDC_DUTY_RES   LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY  5000

static uint32_t clamp_speed_abs(int32_t speed)
{
    if (speed < 0) {
        speed = -speed;
    }
    if (speed > MAX_SPEED) {
        speed = MAX_SPEED;
    }
    return (uint32_t)speed;
}

static void apply_duty(ledc_channel_t channel, uint32_t duty)
{
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

static void set_both_speed(uint32_t speed)
{
    if (speed > MAX_SPEED) {
        speed = MAX_SPEED;
    }

    apply_duty(LEDC_CHANNEL_L, speed);
    apply_duty(LEDC_CHANNEL_R, speed);
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
}

void motor_move_forward(uint32_t speed)
{
    set_left_direction(true);
    set_right_direction(true);
    set_both_speed(speed);
}

void motor_move_backward(uint32_t speed)
{
    set_left_direction(false);
    set_right_direction(false);
    set_both_speed(speed);
}

void motor_turn_left(uint32_t speed)
{
    set_left_direction(false);
    set_right_direction(true);
    set_both_speed(speed);
}

void motor_turn_right(uint32_t speed)
{
    set_left_direction(true);
    set_right_direction(false);
    set_both_speed(speed);
}

void motor_stop(void)
{
    gpio_set_level(MOTOR_L_DIR1_PIN, 0);
    gpio_set_level(MOTOR_L_DIR2_PIN, 0);
    gpio_set_level(MOTOR_R_DIR1_PIN, 0);
    gpio_set_level(MOTOR_R_DIR2_PIN, 0);
    set_both_speed(0);
}

void motor_set_left(int32_t speed)
{
    if (speed == 0) {
        gpio_set_level(MOTOR_L_DIR1_PIN, 0);
        gpio_set_level(MOTOR_L_DIR2_PIN, 0);
        apply_duty(LEDC_CHANNEL_L, 0);
        return;
    }

    set_left_direction(speed > 0);
    apply_duty(LEDC_CHANNEL_L, clamp_speed_abs(speed));
}

void motor_set_right(int32_t speed)
{
    if (speed == 0) {
        gpio_set_level(MOTOR_R_DIR1_PIN, 0);
        gpio_set_level(MOTOR_R_DIR2_PIN, 0);
        apply_duty(LEDC_CHANNEL_R, 0);
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
