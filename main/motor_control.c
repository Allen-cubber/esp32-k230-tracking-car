#include "motor_control.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "encoder.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

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

#define MOTOR_PID_NVS_NAMESPACE           "motor"
#define MOTOR_PID_NVS_KEY                 "pid_cfg"
#define MOTOR_PID_NVS_MAGIC               0x4D504944u
#define MOTOR_PID_NVS_VERSION             1u

typedef struct {
    int32_t target_cmd;
    float measured_pps;
    float integral;
    float prev_error;
    uint32_t applied_duty;
} motor_loop_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    float target_max_pps;
    float kp;
    float ki;
    float kd;
    float integral_limit;
    float correction_limit;
    uint32_t min_duty;
} motor_pid_nvs_record_t;

static motor_loop_t s_left_loop;
static motor_loop_t s_right_loop;
static motor_pid_config_t s_pid_config = {
    .target_max_pps = MOTOR_ENCODER_TARGET_MAX_PPS,
    .kp = MOTOR_SPEED_PID_KP,
    .ki = MOTOR_SPEED_PID_KI,
    .kd = MOTOR_SPEED_PID_KD,
    .integral_limit = MOTOR_SPEED_PID_INTEGRAL_LIMIT,
    .correction_limit = MOTOR_SPEED_PID_CORRECTION_LIMIT,
    .min_duty = MOTOR_CLOSED_LOOP_MIN_DUTY,
};
static portMUX_TYPE s_motor_lock = portMUX_INITIALIZER_UNLOCKED;

static motor_pid_config_t motor_sanitize_pid_config(const motor_pid_config_t *config);
static void motor_apply_pid_config(const motor_pid_config_t *config);
static esp_err_t motor_save_pid_config_to_nvs(const motor_pid_config_t *config);
static esp_err_t motor_load_pid_config_from_nvs(void);

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

static uint32_t motor_loop_compute_duty(motor_loop_t *loop,
                                        const motor_pid_config_t *pid,
                                        int32_t target_cmd,
                                        float measured_pps,
                                        float dt_s)
{
    if (target_cmd == 0) {
        motor_loop_reset(loop);
        return 0;
    }

    const float target_pps = ((float)abs(target_cmd) / (float)MAX_SPEED) *
                             pid->target_max_pps;
    const float error = target_pps - measured_pps;

    loop->integral += error * dt_s;
    loop->integral = clamp_float(loop->integral,
                                 -pid->integral_limit,
                                 pid->integral_limit);

    const float derivative = dt_s > 0.0f ? (error - loop->prev_error) / dt_s : 0.0f;
    loop->prev_error = error;
    loop->measured_pps = measured_pps;

    float correction = pid->kp * error +
                       pid->ki * loop->integral +
                       pid->kd * derivative;
    correction = clamp_float(correction,
                             -pid->correction_limit,
                             pid->correction_limit);

    float duty = (float)abs(target_cmd) + correction;
    if (duty > 0.0f && duty < pid->min_duty) {
        duty = pid->min_duty;
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
        motor_pid_config_t pid;
        portENTER_CRITICAL(&s_motor_lock);
        left_target = s_left_loop.target_cmd;
        right_target = s_right_loop.target_cmd;
        pid = s_pid_config;
        portEXIT_CRITICAL(&s_motor_lock);

        const uint32_t left_duty = motor_loop_compute_duty(&s_left_loop, &pid, left_target, left_pps, dt_s);
        const uint32_t right_duty = motor_loop_compute_duty(&s_right_loop, &pid, right_target, right_pps, dt_s);

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

    motor_pid_config_t pid;
    motor_get_pid_config(&pid);
    ESP_LOGI(TAG,
             "speed closed-loop enabled period=%dms target_max=%.0fpps kp=%.2f ki=%.2f kd=%.2f",
             MOTOR_CLOSED_LOOP_PERIOD_MS,
             pid.target_max_pps,
             pid.kp,
             pid.ki,
             pid.kd);
}

void motor_init(void)
{
    esp_err_t pid_ret = motor_load_pid_config_from_nvs();
    if (pid_ret == ESP_ERR_NVS_NOT_FOUND || pid_ret == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "no saved pid config in NVS, using defaults");
    } else if (pid_ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to load pid config from NVS: %s, using defaults", esp_err_to_name(pid_ret));
    }

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

static motor_pid_config_t motor_sanitize_pid_config(const motor_pid_config_t *config)
{
    return (motor_pid_config_t){
        .target_max_pps = clamp_float(config->target_max_pps, 200.0f, 8000.0f),
        .kp = clamp_float(config->kp, 0.0f, 10.0f),
        .ki = clamp_float(config->ki, 0.0f, 10.0f),
        .kd = clamp_float(config->kd, 0.0f, 10.0f),
        .integral_limit = clamp_float(config->integral_limit, 0.0f, 20000.0f),
        .correction_limit = clamp_float(config->correction_limit, 0.0f, (float)MAX_SPEED),
        .min_duty = (uint32_t)clamp_float((float)config->min_duty, 0.0f, (float)MAX_SPEED),
    };
}

static void motor_apply_pid_config(const motor_pid_config_t *config)
{
    portENTER_CRITICAL(&s_motor_lock);
    s_pid_config = *config;
    s_left_loop.integral = 0.0f;
    s_left_loop.prev_error = 0.0f;
    s_right_loop.integral = 0.0f;
    s_right_loop.prev_error = 0.0f;
    portEXIT_CRITICAL(&s_motor_lock);
}

static esp_err_t motor_save_pid_config_to_nvs(const motor_pid_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(MOTOR_PID_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    const motor_pid_nvs_record_t record = {
        .magic = MOTOR_PID_NVS_MAGIC,
        .version = MOTOR_PID_NVS_VERSION,
        .reserved = 0,
        .target_max_pps = config->target_max_pps,
        .kp = config->kp,
        .ki = config->ki,
        .kd = config->kd,
        .integral_limit = config->integral_limit,
        .correction_limit = config->correction_limit,
        .min_duty = config->min_duty,
    };

    ret = nvs_set_blob(handle, MOTOR_PID_NVS_KEY, &record, sizeof(record));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t motor_load_pid_config_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(MOTOR_PID_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    motor_pid_nvs_record_t record = {0};
    size_t size = sizeof(record);
    ret = nvs_get_blob(handle, MOTOR_PID_NVS_KEY, &record, &size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }
    if (size != sizeof(record) ||
        record.magic != MOTOR_PID_NVS_MAGIC ||
        record.version != MOTOR_PID_NVS_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    const motor_pid_config_t loaded = {
        .target_max_pps = record.target_max_pps,
        .kp = record.kp,
        .ki = record.ki,
        .kd = record.kd,
        .integral_limit = record.integral_limit,
        .correction_limit = record.correction_limit,
        .min_duty = record.min_duty,
    };
    const motor_pid_config_t sanitized = motor_sanitize_pid_config(&loaded);
    motor_apply_pid_config(&sanitized);

    ESP_LOGI(TAG,
             "pid loaded from NVS target_max=%.1f kp=%.3f ki=%.3f kd=%.3f integral=%.1f correction=%.1f min_duty=%lu",
             sanitized.target_max_pps,
             sanitized.kp,
             sanitized.ki,
             sanitized.kd,
             sanitized.integral_limit,
             sanitized.correction_limit,
             (unsigned long)sanitized.min_duty);
    return ESP_OK;
}

void motor_get_status(motor_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_motor_lock);
    out_status->left_target_cmd = s_left_loop.target_cmd;
    out_status->right_target_cmd = s_right_loop.target_cmd;
    out_status->left_measured_pps = s_left_loop.measured_pps;
    out_status->right_measured_pps = s_right_loop.measured_pps;
    out_status->left_applied_duty = s_left_loop.applied_duty;
    out_status->right_applied_duty = s_right_loop.applied_duty;
    portEXIT_CRITICAL(&s_motor_lock);
}

void motor_get_pid_config(motor_pid_config_t *out_config)
{
    if (out_config == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_motor_lock);
    *out_config = s_pid_config;
    portEXIT_CRITICAL(&s_motor_lock);
}

esp_err_t motor_set_pid_config(const motor_pid_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    motor_pid_config_t next = motor_sanitize_pid_config(config);
    motor_apply_pid_config(&next);

    ESP_LOGI(TAG,
             "pid updated target_max=%.1f kp=%.3f ki=%.3f kd=%.3f integral=%.1f correction=%.1f min_duty=%lu",
             next.target_max_pps,
             next.kp,
             next.ki,
             next.kd,
             next.integral_limit,
             next.correction_limit,
             (unsigned long)next.min_duty);

    esp_err_t ret = motor_save_pid_config_to_nvs(&next);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pid applied but failed to save to NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "pid saved to NVS");
    return ESP_OK;
}
