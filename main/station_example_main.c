/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "deepseek_client.h"
#include "k230_uart.h"
#include "motor_control.h"
#include "mpu6050.h"
#include "ultrasonic.h"
#include "vehicle_ai_control.h"
#include "wifi_connection.h"

#define FOLLOW_PAN_CENTER_DEG        90.0f
#define FOLLOW_TILT_CENTER_DEG       90.0f
#define FOLLOW_PAN_DEADBAND_DEG       4.0f
#define FOLLOW_PID_KP                26.0f
#define FOLLOW_PID_KI                 0.0f
#define FOLLOW_PID_KD                0.0f
#define FOLLOW_PID_INTEGRAL_LIMIT   600.0f
#define FOLLOW_HEADING_FILTER_ALPHA   0.35f
#define FOLLOW_MIN_TURN_OUTPUT      520.0f
#define FOLLOW_SPEED_PID_KP          108.0f
#define FOLLOW_SPEED_PID_KI           0.6f
#define FOLLOW_SPEED_PID_KD           0.0f
#define FOLLOW_SPEED_INTEGRAL_LIMIT 800.0f
#define FOLLOW_FORWARD_ACCEL_LIMIT  2600.0f
#define FOLLOW_FORWARD_DECEL_LIMIT  3600.0f
#define FOLLOW_TURN_SLOWDOWN_START_DEG 12.0f
#define FOLLOW_TURN_SLOWDOWN_END_DEG 36.0f
#define FOLLOW_TURN_MIN_FORWARD_SCALE 0.35f
#define FOLLOW_FACE_TARGET_SIZE_PX  145.0f
#define FOLLOW_FACE_SIZE_DEADBAND_PX  8.0f
#define FOLLOW_FACE_FILTER_ALPHA      0.25f
#define FOLLOW_MIN_SPEED            750
#define FOLLOW_MAX_SPEED           5000
#define FOLLOW_MAX_TURN_OUTPUT     2800.0f
#define FOLLOW_TRACK_TIMEOUT_MS     400
#define FOLLOW_FACE_STOP_WIDTH_PX   170.0f
#define FOLLOW_FACE_STOP_HEIGHT_PX  170.0f
#define FOLLOW_TILT_MIN_DEG          50.0f
#define FOLLOW_TILT_MAX_DEG         130.0f
#define FOLLOW_DEBUG_LOG_PERIOD_MS  200
#define OBSTACLE_WARN_DISTANCE_CM    15.0f
#define OBSTACLE_CLEAR_DISTANCE_CM   22.0f
#define OBSTACLE_SAMPLE_PERIOD_MS    80

typedef struct {
    float integral;
    float prev_error;
} follow_pid_state_t;

static const char *TAG = "wifi station";
static const char *POSE_TAG = "pose";
static const char *TRACK_TAG = "track";
static const char *CTRL_TAG = "follow";
static const char *DEBUG_TAG = "follow_debug";

static follow_pid_state_t s_pan_pid = {0};
static follow_pid_state_t s_speed_pid = {0};
static float s_filtered_face_size = 0.0f;
static float s_target_yaw_deg = 0.0f;
static float s_forward_speed_cmd = 0.0f;
static bool s_face_filter_ready = false;
static bool s_target_yaw_ready = false;
static bool s_obstacle_hold = false;
static float s_ultrasonic_distance_cm = -1.0f;
static TickType_t s_last_ultrasonic_sample_tick = 0;
static TickType_t s_last_debug_log_tick = 0;

static float clampf_local(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float normalize_angle_delta(float angle_deg)
{
    while (angle_deg > 180.0f) {
        angle_deg -= 360.0f;
    }
    while (angle_deg < -180.0f) {
        angle_deg += 360.0f;
    }
    return angle_deg;
}

static int32_t clamp_speed_local(int32_t value)
{
    if (value > FOLLOW_MAX_SPEED) {
        return FOLLOW_MAX_SPEED;
    }
    if (value < -FOLLOW_MAX_SPEED) {
        return -FOLLOW_MAX_SPEED;
    }
    return value;
}

static void follow_pid_reset(void)
{
    s_pan_pid.integral = 0.0f;
    s_pan_pid.prev_error = 0.0f;
    s_speed_pid.integral = 0.0f;
    s_speed_pid.prev_error = 0.0f;
    s_filtered_face_size = 0.0f;
    s_target_yaw_deg = 0.0f;
    s_forward_speed_cmd = 0.0f;
    s_face_filter_ready = false;
    s_target_yaw_ready = false;
}

static bool follow_face_too_large(const k230_track_data_t *track)
{
    return (track->face_w >= FOLLOW_FACE_STOP_WIDTH_PX) ||
           (track->face_h >= FOLLOW_FACE_STOP_HEIGHT_PX);
}

static bool follow_tilt_out_of_range(float tilt_deg)
{
    return (tilt_deg < FOLLOW_TILT_MIN_DEG) || (tilt_deg > FOLLOW_TILT_MAX_DEG);
}

static float follow_update_face_size_filter(const k230_track_data_t *track)
{
    float face_size = fmaxf(track->face_w, track->face_h);

    if (!s_face_filter_ready) {
        s_filtered_face_size = face_size;
        s_face_filter_ready = true;
        return s_filtered_face_size;
    }

    s_filtered_face_size += FOLLOW_FACE_FILTER_ALPHA * (face_size - s_filtered_face_size);
    return s_filtered_face_size;
}

static int32_t follow_calculate_forward_speed(float face_size, float dt_s)
{
    float size_error = FOLLOW_FACE_TARGET_SIZE_PX - face_size;
    float derivative;
    float speed_output;

    if (fabsf(size_error) < FOLLOW_FACE_SIZE_DEADBAND_PX) {
        s_speed_pid.integral = 0.0f;
        s_speed_pid.prev_error = 0.0f;
        return 0;
    }

    if (size_error < 0.0f) {
        s_speed_pid.integral = 0.0f;
        s_speed_pid.prev_error = size_error;
        return 0;
    }

    s_speed_pid.integral += size_error * dt_s;
    s_speed_pid.integral = clampf_local(
        s_speed_pid.integral,
        -FOLLOW_SPEED_INTEGRAL_LIMIT,
        FOLLOW_SPEED_INTEGRAL_LIMIT
    );

    derivative = (size_error - s_speed_pid.prev_error) / dt_s;
    speed_output = (FOLLOW_SPEED_PID_KP * size_error) +
                   (FOLLOW_SPEED_PID_KI * s_speed_pid.integral) +
                   (FOLLOW_SPEED_PID_KD * derivative);
    s_speed_pid.prev_error = size_error;

    speed_output = clampf_local(speed_output, 0.0f, FOLLOW_MAX_SPEED);
    if ((speed_output > 0.0f) && (speed_output < FOLLOW_MIN_SPEED)) {
        speed_output = FOLLOW_MIN_SPEED;
    }

    return (int32_t)speed_output;
}

static int32_t follow_apply_forward_accel_limit(int32_t target_speed, float dt_s)
{
    float max_delta;
    float delta = (float)target_speed - s_forward_speed_cmd;
    float limit = (delta > 0.0f) ? FOLLOW_FORWARD_ACCEL_LIMIT : FOLLOW_FORWARD_DECEL_LIMIT;

    max_delta = limit * dt_s;
    delta = clampf_local(delta, -max_delta, max_delta);
    s_forward_speed_cmd += delta;

    if (s_forward_speed_cmd < 1.0f) {
        s_forward_speed_cmd = 0.0f;
    }

    return (int32_t)s_forward_speed_cmd;
}

static int32_t follow_apply_turn_priority(int32_t forward_speed, float heading_error_deg)
{
    float abs_error = fabsf(heading_error_deg);
    float scale;

    if (abs_error <= FOLLOW_TURN_SLOWDOWN_START_DEG) {
        return forward_speed;
    }

    if (abs_error >= FOLLOW_TURN_SLOWDOWN_END_DEG) {
        scale = FOLLOW_TURN_MIN_FORWARD_SCALE;
    } else {
        scale = (FOLLOW_TURN_SLOWDOWN_END_DEG - abs_error) /
                (FOLLOW_TURN_SLOWDOWN_END_DEG - FOLLOW_TURN_SLOWDOWN_START_DEG);
        scale = clampf_local(scale, FOLLOW_TURN_MIN_FORWARD_SCALE, 1.0f);
    }

    return (int32_t)((float)forward_speed * scale);
}

static bool obstacle_is_valid(float distance_cm)
{
    return distance_cm > 0.0f;
}

static float obstacle_update_distance(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t period_ticks = pdMS_TO_TICKS(OBSTACLE_SAMPLE_PERIOD_MS);

    if ((s_last_ultrasonic_sample_tick == 0) ||
        ((now - s_last_ultrasonic_sample_tick) >= period_ticks)) {
        s_ultrasonic_distance_cm = ultrasonic_measure_cm();
        s_last_ultrasonic_sample_tick = now;
    }

    return s_ultrasonic_distance_cm;
}

static bool obstacle_guard_should_stop(float distance_cm)
{
    if (!obstacle_is_valid(distance_cm)) {
        return s_obstacle_hold;
    }

    if (s_obstacle_hold) {
        if (distance_cm >= OBSTACLE_CLEAR_DISTANCE_CM) {
            s_obstacle_hold = false;
            ESP_LOGI(CTRL_TAG, "obstacle cleared: %.2f cm -> resume follow", distance_cm);
        }
    } else if (distance_cm <= OBSTACLE_WARN_DISTANCE_CM) {
        s_obstacle_hold = true;
        ESP_LOGW(CTRL_TAG, "obstacle warning: %.2f cm <= %.2f cm -> stop and hold",
                 distance_cm,
                 OBSTACLE_WARN_DISTANCE_CM);
    }

    if (s_obstacle_hold) {
        follow_pid_reset();
        motor_stop();
    }

    return s_obstacle_hold;
}

static void follow_log_control_debug(const k230_track_data_t *track,
                                     const mpu6050_pose_t *pose,
                                     bool pose_ready,
                                     float ultrasonic_cm,
                                     float face_size,
                                     int32_t target_speed,
                                     int32_t base_speed,
                                     float heading_error_deg,
                                     float turn_output,
                                     int32_t left_speed,
                                     int32_t right_speed)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t period_ticks = pdMS_TO_TICKS(FOLLOW_DEBUG_LOG_PERIOD_MS);

    if ((s_last_debug_log_tick != 0) && ((now - s_last_debug_log_tick) < period_ticks)) {
        return;
    }
    s_last_debug_log_tick = now;

    ESP_LOGI(DEBUG_TAG,
             "K230 pan=%.2f tilt=%.2f face=%.1fx%.1f size=%.1f | ultra=%.2fcm | MPU ready=%d yaw=%.2f yaw_rate=%.2f | steer target_yaw=%.2f err=%.2f turn=%.2f | speed target=%ld cmd=%ld L=%ld R=%ld",
             track->pan_deg,
             track->tilt_deg,
             track->face_w,
             track->face_h,
             face_size,
             ultrasonic_cm,
             pose_ready ? 1 : 0,
             pose_ready ? pose->yaw_deg : 0.0f,
             pose_ready ? pose->yaw_rate_dps : 0.0f,
             pose_ready ? s_target_yaw_deg : 0.0f,
             heading_error_deg,
             turn_output,
             (long)target_speed,
             (long)base_speed,
             (long)left_speed,
             (long)right_speed);
}

static float follow_calculate_heading_error(const k230_track_data_t *track,
                                            const mpu6050_pose_t *pose,
                                            bool pose_ready)
{
    float pan_error_deg = track->pan_deg - FOLLOW_PAN_CENTER_DEG;
    float observed_target_yaw_deg;

    if (!pose_ready) {
        s_target_yaw_ready = false;
        return pan_error_deg;
    }

    observed_target_yaw_deg = pose->yaw_deg + pan_error_deg;
    if (!s_target_yaw_ready) {
        s_target_yaw_deg = observed_target_yaw_deg;
        s_target_yaw_ready = true;
    } else {
        s_target_yaw_deg += FOLLOW_HEADING_FILTER_ALPHA *
                            normalize_angle_delta(observed_target_yaw_deg - s_target_yaw_deg);
    }

    return normalize_angle_delta(s_target_yaw_deg - pose->yaw_deg);
}

static void follow_target_control(const k230_track_data_t *track,
                                  const mpu6050_pose_t *pose,
                                  bool pose_ready,
                                  float ultrasonic_cm,
                                  float dt_s)
{
    float heading_error_deg;
    float effective_error_deg;
    float derivative;
    float turn_output;
    float face_size;
    int32_t target_speed;
    int32_t base_speed;
    int32_t left_speed;
    int32_t right_speed;

    if (!track->valid) {
        ESP_LOGI(CTRL_TAG, "target lost -> stop");
        follow_pid_reset();
        motor_stop();
        return;
    }

    if (follow_face_too_large(track)) {
        ESP_LOGI(CTRL_TAG,
                 "face too close (w=%.1f h=%.1f limit=%.1fx%.1f) -> stop",
                 track->face_w,
                 track->face_h,
                 FOLLOW_FACE_STOP_WIDTH_PX,
                 FOLLOW_FACE_STOP_HEIGHT_PX);
        follow_pid_reset();
        motor_stop();
        return;
    }

    if (follow_tilt_out_of_range(track->tilt_deg)) {
        ESP_LOGI(CTRL_TAG,
                 "tilt unsafe (tilt=%.2f safe=%.1f..%.1f) -> stop",
                 track->tilt_deg,
                 FOLLOW_TILT_MIN_DEG,
                 FOLLOW_TILT_MAX_DEG);
        follow_pid_reset();
        motor_stop();
        return;
    }

    face_size = follow_update_face_size_filter(track);

    heading_error_deg = follow_calculate_heading_error(track, pose, pose_ready);
    effective_error_deg = (fabsf(heading_error_deg) < FOLLOW_PAN_DEADBAND_DEG) ?
                          0.0f :
                          heading_error_deg;

    s_pan_pid.integral += effective_error_deg * dt_s;
    s_pan_pid.integral = clampf_local(
        s_pan_pid.integral,
        -FOLLOW_PID_INTEGRAL_LIMIT,
        FOLLOW_PID_INTEGRAL_LIMIT
    );

    derivative = (effective_error_deg - s_pan_pid.prev_error) / dt_s;
    turn_output = (FOLLOW_PID_KP * effective_error_deg) +
                  (FOLLOW_PID_KI * s_pan_pid.integral) +
                  (FOLLOW_PID_KD * derivative);
    if ((effective_error_deg != 0.0f) && (fabsf(turn_output) < FOLLOW_MIN_TURN_OUTPUT)) {
        turn_output = (turn_output < 0.0f) ? -FOLLOW_MIN_TURN_OUTPUT : FOLLOW_MIN_TURN_OUTPUT;
    }
    turn_output = clampf_local(turn_output, -FOLLOW_MAX_TURN_OUTPUT, FOLLOW_MAX_TURN_OUTPUT);
    s_pan_pid.prev_error = effective_error_deg;

    target_speed = follow_calculate_forward_speed(face_size, dt_s);
    target_speed = follow_apply_turn_priority(target_speed, effective_error_deg);
    base_speed = follow_apply_forward_accel_limit(target_speed, dt_s);
    left_speed = clamp_speed_local((int32_t)(base_speed - turn_output));
    right_speed = clamp_speed_local((int32_t)(base_speed + turn_output));

    follow_log_control_debug(track,
                             pose,
                             pose_ready,
                             ultrasonic_cm,
                             face_size,
                             target_speed,
                             base_speed,
                             effective_error_deg,
                             turn_output,
                             left_speed,
                             right_speed);

    motor_set_differential(left_speed, right_speed);
}

void app_main(void)
{
    bool uart_ready = false;
    bool mpu_ready = false;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "before wifi_connection_start");
    ret = wifi_connection_start();
    if (ret == ESP_OK) {
        ret = wifi_connection_wait_connected(pdMS_TO_TICKS(CONFIG_ESP_WIFI_CONNECT_TIMEOUT_MS));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "network ready");
            deepseek_client_start_boot_test();
        } else {
            ESP_LOGW(TAG, "Wi-Fi not ready yet: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "wifi_connection_start failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "before motor_init");
    motor_init();
    ESP_LOGI(TAG, "after motor_init");

    ESP_LOGI(TAG, "before ultrasonic_init");
    ultrasonic_init();
    ESP_LOGI(TAG, "after ultrasonic_init");

    ESP_LOGI(TAG, "before k230_uart_init");
    ret = k230_uart_init();
    if (ret == ESP_OK) {
        uart_ready = true;
        ESP_LOGI(TAG, "after k230_uart_init");
    } else {
        ESP_LOGE(TAG, "k230_uart_init failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "before vehicle_ai_control_init");
    ret = vehicle_ai_control_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "after vehicle_ai_control_init");
    } else {
        ESP_LOGE(TAG, "vehicle_ai_control_init failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "before mpu6050_init");
    ret = mpu6050_init();
    if (ret == ESP_OK) {
        mpu_ready = true;
        ESP_LOGI(TAG, "after mpu6050_init");
    } else {
        ESP_LOGE(TAG, "mpu6050_init failed: %s", esp_err_to_name(ret));
    }

    if (mpu_ready) {
        ESP_LOGI(TAG, "before mpu6050_calibrate");
        ret = mpu6050_calibrate_gyro(CONFIG_MPU6050_CALIBRATION_SAMPLES);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "after mpu6050_calibrate");
        } else {
            ESP_LOGE(TAG, "mpu6050_calibrate failed: %s", esp_err_to_name(ret));
            mpu_ready = false;
        }
    }

    const TickType_t sample_delay = pdMS_TO_TICKS(CONFIG_MPU6050_SAMPLE_PERIOD_MS);
    const TickType_t track_timeout = pdMS_TO_TICKS(FOLLOW_TRACK_TIMEOUT_MS);
    const float sample_dt_s = CONFIG_MPU6050_SAMPLE_PERIOD_MS / 1000.0f;

    k230_track_data_t last_track = {0};
    TickType_t last_track_tick = 0;

    ESP_LOGI(CTRL_TAG, "follow mode enabled");

    while (1) {
        mpu6050_pose_t pose = {0};
        k230_track_data_t track = {0};
        bool pose_ready = false;
        float ultrasonic_cm = obstacle_update_distance();

        vehicle_ai_control_update();

        if (mpu_ready) {
            esp_err_t pose_ret = mpu6050_update_pose(&pose, sample_dt_s);

            if (pose_ret == ESP_OK) {
                pose_ready = pose.calibrated;
                ESP_LOGD(POSE_TAG, "yaw=%.2f deg yaw_rate=%.2f dps",
                         pose.yaw_deg, pose.yaw_rate_dps);
            } else {
                ESP_LOGW(POSE_TAG, "mpu6050 update failed: %s", esp_err_to_name(pose_ret));
            }
        }

        if (uart_ready) {
            for (int packet_index = 0; packet_index < 4; ++packet_index) {
                k230_uart_packet_t packet = {0};

                if (!k230_uart_read_packet(&packet)) {
                    break;
                }

                if (packet.type == K230_UART_PACKET_TRACK) {
                    track = packet.track;
                    last_track = track;
                    last_track_tick = xTaskGetTickCount();
                    ESP_LOGD(TRACK_TAG,
                             "valid=%d pan=%.2f tilt=%.2f face=(%.1f, %.1f, %.1f, %.1f)",
                             track.valid,
                             track.pan_deg,
                             track.tilt_deg,
                             track.face_x,
                             track.face_y,
                             track.face_w,
                             track.face_h);
                } else if (packet.type == K230_UART_PACKET_TEXT) {
                    vehicle_ai_control_submit_text(packet.text);
                }
            }
        }

        if ((last_track_tick != 0) && ((xTaskGetTickCount() - last_track_tick) > track_timeout)) {
            last_track.valid = false;
        }

        if (obstacle_guard_should_stop(ultrasonic_cm)) {
            ESP_LOGD(CTRL_TAG, "obstacle hold, ultrasonic=%.2f cm", ultrasonic_cm);
        } else if (vehicle_ai_control_is_manual_mode()) {
            ESP_LOGD(CTRL_TAG, "AI manual mode active, follow control paused");
        } else if (last_track_tick == 0) {
            follow_pid_reset();
            motor_stop();
            ESP_LOGI(CTRL_TAG, "waiting first track packet");
        } else {
            follow_target_control(&last_track, &pose, pose_ready, ultrasonic_cm, sample_dt_s);
        }

        vTaskDelay(sample_delay);
    }
}
