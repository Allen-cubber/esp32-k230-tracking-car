#include "vehicle_ai_control.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "deepseek_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "motor_control.h"
#include "robot_mqtt_client.h"

typedef struct {
    char text[CONFIG_AI_CONTROL_INPUT_MAX_BYTES];
} ai_text_request_t;

static const char *TAG = "ai_control";

static const char *AI_COMMAND_SYSTEM_PROMPT =
    "You convert natural-language smart-car commands into strict JSON only. "
    "The user may speak Chinese or English. Output no markdown and no explanation. "
    "Schema: {"
    "\"chassis\":{\"action\":\"none|forward|backward|turn_left|turn_right|stop|hold|resume_follow\","
    "\"speed\":0-100,\"duration_ms\":0-5000},"
    "\"gimbal\":{\"action\":\"none|nod|shake|lock|unlock|center|up|down|left|right|stop\","
    "\"pan_delta_deg\":-60..60,\"tilt_delta_deg\":-60..60,\"duration_ms\":0-5000},"
    "\"reply\":\"short Chinese confirmation\"}. "
    "Use chassis stop or hold for stop-still requests. Use resume_follow when the user asks to follow/track again. "
    "Use gimbal nod for dian tou/nod, shake for yao tou/shake head, lock for hold-still, unlock for resume camera tracking. "
    "For ordinary questions, answer in reply and keep chassis.action none. "
    "For clear yes/no questions, use gimbal.action nod when the answer is yes/affirmative, "
    "or shake when the answer is no/negative; nod/shake are temporary gestures and camera tracking resumes after them. "
    "Example: if asked whether clean water is poisonous, reply that normal clean water is not poisonous and use gimbal shake. "
    "For movement commands, choose a safe speed around 35 unless the user specifies speed. "
    "For movement duration, choose 1000 ms unless the user specifies distance or time.";

static QueueHandle_t s_request_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_manual_mode;
static bool s_motion_timed;
static TickType_t s_motion_end_tick;

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static bool tick_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline)) >= 0;
}

static void set_manual_mode(bool enabled)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_manual_mode = enabled;
    if (!enabled) {
        s_motion_timed = false;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

static void start_timed_motion(uint32_t duration_ms)
{
    TickType_t now = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_state_lock);
    s_manual_mode = true;
    s_motion_timed = true;
    s_motion_end_tick = now + pdMS_TO_TICKS(duration_ms);
    taskEXIT_CRITICAL(&s_state_lock);
}

static void hold_manual_mode(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_manual_mode = true;
    s_motion_timed = false;
    taskEXIT_CRITICAL(&s_state_lock);
}

bool vehicle_ai_control_is_manual_mode(void)
{
    bool manual_mode;

    taskENTER_CRITICAL(&s_state_lock);
    manual_mode = s_manual_mode;
    taskEXIT_CRITICAL(&s_state_lock);

    return manual_mode;
}

void vehicle_ai_control_update(void)
{
    bool should_stop = false;
    TickType_t now = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_state_lock);
    if (s_motion_timed && tick_reached(now, s_motion_end_tick)) {
        s_motion_timed = false;
        should_stop = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (should_stop) {
        motor_stop();
        ESP_LOGI(TAG, "timed chassis command finished; holding manual mode");
    }
}

static uint32_t speed_percent_to_pwm(int speed_percent)
{
    speed_percent = clamp_int(speed_percent, 0, 100);
    return (uint32_t)((CONFIG_AI_CONTROL_MAX_MOTOR_SPEED * speed_percent) / 100);
}

static const char *json_get_string(const cJSON *object, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return fallback;
}

static int json_get_int(const cJSON *object, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return fallback;
}

static bool action_is(const char *action, const char *expected)
{
    return action != NULL && strcasecmp(action, expected) == 0;
}

static void execute_chassis_command(const cJSON *chassis)
{
    if (!cJSON_IsObject(chassis)) {
        return;
    }

    const char *action = json_get_string(chassis, "action", "none");
    int speed_percent = json_get_int(chassis, "speed", CONFIG_AI_CONTROL_DEFAULT_SPEED_PERCENT);
    int duration_ms = json_get_int(chassis, "duration_ms", CONFIG_AI_CONTROL_DEFAULT_DURATION_MS);

    if (action_is(action, "none")) {
        return;
    }

    if (action_is(action, "resume_follow")) {
        motor_stop();
        set_manual_mode(false);
        ESP_LOGI(TAG, "chassis resume follow mode");
        return;
    }

    hold_manual_mode();

    if (action_is(action, "stop") || action_is(action, "hold")) {
        motor_stop();
        ESP_LOGI(TAG, "chassis %s", action);
        return;
    }

    speed_percent = clamp_int(speed_percent, 1, 100);
    duration_ms = clamp_int(duration_ms,
                            CONFIG_AI_CONTROL_MIN_DURATION_MS,
                            CONFIG_AI_CONTROL_MAX_DURATION_MS);

    uint32_t pwm = speed_percent_to_pwm(speed_percent);

    if (action_is(action, "forward")) {
        motor_move_forward(pwm);
    } else if (action_is(action, "backward")) {
        motor_move_backward(pwm);
    } else if (action_is(action, "turn_left") || action_is(action, "left")) {
        motor_turn_left(pwm);
    } else if (action_is(action, "turn_right") || action_is(action, "right")) {
        motor_turn_right(pwm);
    } else {
        ESP_LOGW(TAG, "unknown chassis action: %s", action);
        return;
    }

    start_timed_motion((uint32_t)duration_ms);
    ESP_LOGI(TAG,
             "chassis action=%s speed=%d%% pwm=%lu duration=%dms",
             action,
             speed_percent,
             (unsigned long)pwm,
             duration_ms);
}

static void execute_gimbal_command(const cJSON *gimbal)
{
    if (!cJSON_IsObject(gimbal)) {
        return;
    }

    const char *action = json_get_string(gimbal, "action", "none");
    if (action_is(action, "none")) {
        return;
    }

    int default_step = CONFIG_AI_CONTROL_GIMBAL_STEP_DEG;
    int pan_delta = json_get_int(gimbal, "pan_delta_deg", 0);
    int tilt_delta = json_get_int(gimbal, "tilt_delta_deg", 0);
    int duration_ms = json_get_int(gimbal, "duration_ms", CONFIG_AI_CONTROL_GIMBAL_DEFAULT_DURATION_MS);

    if (action_is(action, "left") && pan_delta == 0) {
        pan_delta = -default_step;
    } else if (action_is(action, "right") && pan_delta == 0) {
        pan_delta = default_step;
    } else if (action_is(action, "up") && tilt_delta == 0) {
        tilt_delta = -default_step;
    } else if (action_is(action, "down") && tilt_delta == 0) {
        tilt_delta = default_step;
    } else if (action_is(action, "nod") && tilt_delta == 0) {
        tilt_delta = default_step;
    } else if (action_is(action, "shake") && pan_delta == 0) {
        pan_delta = default_step;
    }

    pan_delta = clamp_int(pan_delta, -CONFIG_AI_CONTROL_GIMBAL_MAX_DELTA_DEG,
                         CONFIG_AI_CONTROL_GIMBAL_MAX_DELTA_DEG);
    tilt_delta = clamp_int(tilt_delta, -CONFIG_AI_CONTROL_GIMBAL_MAX_DELTA_DEG,
                          CONFIG_AI_CONTROL_GIMBAL_MAX_DELTA_DEG);
    duration_ms = clamp_int(duration_ms, 0, CONFIG_AI_CONTROL_MAX_DURATION_MS);

    esp_err_t ret = robot_mqtt_client_send_gimbal_command(action,
                                                          pan_delta,
                                                          tilt_delta,
                                                          (uint32_t)duration_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send gimbal command failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG,
             "gimbal action=%s pan_delta=%d tilt_delta=%d duration=%dms",
             action,
             pan_delta,
             tilt_delta,
             duration_ms);
}

#define DIRECT_ACTION_MAX_SPEED_PERCENT 50
#define DIRECT_ACTION_MAX_DURATION_MS 3000
#define DIRECT_ACTION_GIMBAL_MAX_DELTA_DEG 45

static esp_err_t direct_action_error(char *error_buf,
                                     size_t error_buf_size,
                                     const char *error)
{
    if (error_buf != NULL && error_buf_size > 0) {
        snprintf(error_buf, error_buf_size, "%s", error);
    }
    return ESP_ERR_INVALID_ARG;
}

static int direct_action_motion_speed(int speed_percent)
{
    int default_speed = clamp_int(CONFIG_AI_CONTROL_DEFAULT_SPEED_PERCENT,
                                  1,
                                  DIRECT_ACTION_MAX_SPEED_PERCENT);

    speed_percent = clamp_int(speed_percent, 0, DIRECT_ACTION_MAX_SPEED_PERCENT);
    if (speed_percent == 0) {
        speed_percent = default_speed;
    }

    return speed_percent;
}

static int direct_action_motion_duration(int duration_ms)
{
    int default_duration = clamp_int(CONFIG_AI_CONTROL_DEFAULT_DURATION_MS,
                                     CONFIG_AI_CONTROL_MIN_DURATION_MS,
                                     DIRECT_ACTION_MAX_DURATION_MS);

    duration_ms = clamp_int(duration_ms, 0, DIRECT_ACTION_MAX_DURATION_MS);
    if (duration_ms == 0) {
        duration_ms = default_duration;
    }

    return clamp_int(duration_ms,
                     CONFIG_AI_CONTROL_MIN_DURATION_MS,
                     DIRECT_ACTION_MAX_DURATION_MS);
}

static esp_err_t execute_direct_chassis_action(const char *name,
                                               int speed_percent,
                                               int duration_ms,
                                               char *error_buf,
                                               size_t error_buf_size)
{
    if (action_is(name, "stop")) {
        hold_manual_mode();
        motor_stop();
        ESP_LOGI(TAG, "direct chassis stop");
        return ESP_OK;
    }

    if (!action_is(name, "forward") &&
        !action_is(name, "backward") &&
        !action_is(name, "turn_left") &&
        !action_is(name, "turn_right")) {
        return direct_action_error(error_buf, error_buf_size, "unknown_action");
    }

    speed_percent = direct_action_motion_speed(speed_percent);
    duration_ms = direct_action_motion_duration(duration_ms);
    uint32_t pwm = speed_percent_to_pwm(speed_percent);

    hold_manual_mode();
    if (action_is(name, "forward")) {
        motor_move_forward(pwm);
    } else if (action_is(name, "backward")) {
        motor_move_backward(pwm);
    } else if (action_is(name, "turn_left")) {
        motor_turn_left(pwm);
    } else {
        motor_turn_right(pwm);
    }
    start_timed_motion((uint32_t)duration_ms);

    ESP_LOGI(TAG,
             "direct chassis action=%s speed=%d%% pwm=%lu duration=%dms",
             name,
             speed_percent,
             (unsigned long)pwm,
             duration_ms);
    return ESP_OK;
}

static esp_err_t execute_direct_mode_action(const char *name,
                                            char *error_buf,
                                            size_t error_buf_size)
{
    if (action_is(name, "hold")) {
        hold_manual_mode();
        motor_stop();
        ESP_LOGI(TAG, "direct mode hold");
        return ESP_OK;
    }

    if (action_is(name, "resume_follow")) {
        motor_stop();
        set_manual_mode(false);
        ESP_LOGI(TAG, "direct mode resume follow");
        return ESP_OK;
    }

    return direct_action_error(error_buf, error_buf_size, "unknown_action");
}

static esp_err_t execute_direct_gimbal_action(const char *name,
                                              int pan_delta,
                                              int tilt_delta,
                                              int duration_ms,
                                              char *error_buf,
                                              size_t error_buf_size)
{
    int default_step = CONFIG_AI_CONTROL_GIMBAL_STEP_DEG;

    if (!action_is(name, "nod") &&
        !action_is(name, "shake") &&
        !action_is(name, "center") &&
        !action_is(name, "lock") &&
        !action_is(name, "unlock")) {
        return direct_action_error(error_buf, error_buf_size, "unknown_action");
    }

    if (action_is(name, "nod") && tilt_delta == 0) {
        tilt_delta = default_step;
    } else if (action_is(name, "shake") && pan_delta == 0) {
        pan_delta = default_step;
    }

    pan_delta = clamp_int(pan_delta,
                          -DIRECT_ACTION_GIMBAL_MAX_DELTA_DEG,
                          DIRECT_ACTION_GIMBAL_MAX_DELTA_DEG);
    tilt_delta = clamp_int(tilt_delta,
                           -DIRECT_ACTION_GIMBAL_MAX_DELTA_DEG,
                           DIRECT_ACTION_GIMBAL_MAX_DELTA_DEG);
    duration_ms = clamp_int(duration_ms, 0, DIRECT_ACTION_MAX_DURATION_MS);

    esp_err_t ret = robot_mqtt_client_send_gimbal_command(name,
                                                          pan_delta,
                                                          tilt_delta,
                                                          (uint32_t)duration_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "direct gimbal command failed: %s", esp_err_to_name(ret));
        return direct_action_error(error_buf, error_buf_size, "gimbal_send_failed");
    }

    ESP_LOGI(TAG,
             "direct gimbal action=%s pan_delta=%d tilt_delta=%d duration=%dms",
             name,
             pan_delta,
             tilt_delta,
             duration_ms);
    return ESP_OK;
}

esp_err_t vehicle_ai_control_execute_action_payload(const cJSON *payload,
                                                    char *error_buf,
                                                    size_t error_buf_size)
{
    if (!cJSON_IsObject(payload)) {
        return direct_action_error(error_buf, error_buf_size, "payload_not_object");
    }

    const cJSON *action = cJSON_GetObjectItemCaseSensitive(payload, "action");
    if (!cJSON_IsObject(action)) {
        return direct_action_error(error_buf, error_buf_size, "action_not_object");
    }

    const char *target = json_get_string(action, "target", "none");
    const char *name = json_get_string(action, "name", "none");
    int speed_percent = json_get_int(action, "speed", 0);
    int duration_ms = json_get_int(action, "duration_ms", 0);
    int pan_delta = json_get_int(action, "pan_delta_deg", 0);
    int tilt_delta = json_get_int(action, "tilt_delta_deg", 0);

    if (action_is(target, "none")) {
        if (!action_is(name, "none")) {
            return direct_action_error(error_buf, error_buf_size, "unknown_action");
        }
        ESP_LOGI(TAG, "direct action none");
        return ESP_OK;
    }

    if (action_is(target, "chassis")) {
        return execute_direct_chassis_action(name,
                                             speed_percent,
                                             duration_ms,
                                             error_buf,
                                             error_buf_size);
    }

    if (action_is(target, "mode")) {
        return execute_direct_mode_action(name, error_buf, error_buf_size);
    }

    if (action_is(target, "gimbal")) {
        return execute_direct_gimbal_action(name,
                                            pan_delta,
                                            tilt_delta,
                                            duration_ms,
                                            error_buf,
                                            error_buf_size);
    }

    return direct_action_error(error_buf, error_buf_size, "unknown_target");
}

static char *extract_json_object(const char *text)
{
    const char *start = strchr(text, '{');
    const char *end = strrchr(text, '}');

    if (start == NULL || end == NULL || end < start) {
        return NULL;
    }

    size_t len = (size_t)(end - start + 1);
    char *json = malloc(len + 1);
    if (json == NULL) {
        return NULL;
    }

    memcpy(json, start, len);
    json[len] = '\0';
    return json;
}

static esp_err_t execute_ai_reply(const char *reply)
{
    char *json_text = extract_json_object(reply);
    if (json_text == NULL) {
        ESP_LOGE(TAG, "AI reply does not contain JSON: %s", reply);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(json_text);
    free(json_text);

    if (root == NULL) {
        ESP_LOGE(TAG, "AI command JSON parse failed: %s", reply);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *chassis = cJSON_GetObjectItemCaseSensitive(root, "chassis");
    const cJSON *gimbal = cJSON_GetObjectItemCaseSensitive(root, "gimbal");
    const char *reply_text = json_get_string(root, "reply", "");

    execute_chassis_command(chassis);
    execute_gimbal_command(gimbal);

    if (reply_text[0] != '\0') {
        ESP_LOGI(TAG, "AI confirmation: %s", reply_text);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void trim_line(char *line)
{
    size_t len = strlen(line);

    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[len - 1] = '\0';
        len--;
    }

    char *start = line;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }
}

esp_err_t vehicle_ai_control_submit_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_request_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ai_text_request_t request = {0};
    snprintf(request.text, sizeof(request.text), "%s", text);
    trim_line(request.text);

    if (request.text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        ESP_LOGW(TAG, "AI command queue full, dropped: %s", request.text);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "queued text command: %s", request.text);
    return ESP_OK;
}

static void ai_command_task(void *arg)
{
    ai_text_request_t request;
    char reply[CONFIG_AI_CONTROL_REPLY_MAX_BYTES];

    while (1) {
        if (xQueueReceive(s_request_queue, &request, portMAX_DELAY) != pdPASS) {
            continue;
        }

        memset(reply, 0, sizeof(reply));
        ESP_LOGI(TAG, "asking DeepSeek: %s", request.text);

        esp_err_t ret = deepseek_client_chat_with_system(AI_COMMAND_SYSTEM_PROMPT,
                                                         request.text,
                                                         reply,
                                                         sizeof(reply));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "DeepSeek command request failed: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "DeepSeek command JSON: %s", reply);

        ret = execute_ai_reply(reply);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "execute AI reply failed: %s", esp_err_to_name(ret));
        }
    }
}

#ifdef CONFIG_AI_CONTROL_SERIAL_INPUT_ENABLE
static void serial_input_task(void *arg)
{
    char line[CONFIG_AI_CONTROL_INPUT_MAX_BYTES];

    ESP_LOGI(TAG, "serial AI input enabled; type text and press Enter");

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        trim_line(line);
        if (line[0] != '\0') {
            vehicle_ai_control_submit_text(line);
        }
    }
}
#endif

esp_err_t vehicle_ai_control_init(void)
{
    if (s_request_queue != NULL) {
        return ESP_OK;
    }

    s_request_queue = xQueueCreate(CONFIG_AI_CONTROL_QUEUE_LEN, sizeof(ai_text_request_t));
    if (s_request_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(ai_command_task,
                                     "ai_cmd",
                                     CONFIG_AI_CONTROL_TASK_STACK_SIZE,
                                     NULL,
                                     5,
                                     NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

#ifdef CONFIG_AI_CONTROL_SERIAL_INPUT_ENABLE
    created = xTaskCreate(serial_input_task,
                          "ai_serial",
                          CONFIG_AI_CONTROL_SERIAL_TASK_STACK_SIZE,
                          NULL,
                          4,
                          NULL);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "failed to create serial input task");
    }
#endif

    ESP_LOGI(TAG, "AI vehicle control ready");
    return ESP_OK;
}
