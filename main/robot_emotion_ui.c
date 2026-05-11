#include "robot_emotion_ui.h"

#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "robot_face_ui.h"
#include "robot_lcd.h"

#define UI_LOCK_TIMEOUT_MS 30

static const char *TAG = "emotion_ui";

static bool s_ui_ready;
static lv_obj_t *s_screen;

robot_emotion_t robot_emotion_from_string(const char *emotion)
{
    if (emotion == NULL) {
        return ROBOT_EMOTION_NEUTRAL;
    }
    if (strcasecmp(emotion, "happy") == 0) {
        return ROBOT_EMOTION_HAPPY;
    }
    if (strcasecmp(emotion, "alert") == 0) {
        return ROBOT_EMOTION_ALERT;
    }
    if (strcasecmp(emotion, "confused") == 0) {
        return ROBOT_EMOTION_CONFUSED;
    }
    if (strcasecmp(emotion, "sad") == 0) {
        return ROBOT_EMOTION_SAD;
    }
    return ROBOT_EMOTION_NEUTRAL;
}

static bool status_is(const char *status, const char *value)
{
    return status != NULL && strcasecmp(status, value) == 0;
}

static bool status_has(const char *status, const char *needle)
{
    return status != NULL && strstr(status, needle) != NULL;
}

static robot_ui_state_t map_status_state(robot_emotion_t emotion, const char *status)
{
    if (status_is(status, "BOOT")) {
        return ROBOT_UI_BOOT;
    }
    if (status_is(status, "FOLLOW")) {
        return ROBOT_UI_FOLLOW;
    }
    if (status_is(status, "LOST")) {
        return ROBOT_UI_LOST;
    }
    if (status_is(status, "WAIT TRACK") || status_is(status, "WAIT_TRACK")) {
        return ROBOT_UI_WAIT_TRACK;
    }
    if (status_is(status, "SEARCH")) {
        return ROBOT_UI_SEARCH;
    }
    if (status_is(status, "OBSTACLE")) {
        return ROBOT_UI_OBSTACLE;
    }
    if (status_is(status, "TOO CLOSE") || status_is(status, "TOO_CLOSE")) {
        return ROBOT_UI_TOO_CLOSE;
    }
    if (status_is(status, "TILT LIMIT") || status_is(status, "TILT_LIMIT")) {
        return ROBOT_UI_TILT_LIMIT;
    }
    if (status_is(status, "HOLD")) {
        return ROBOT_UI_HOLD;
    }

    switch (emotion) {
    case ROBOT_EMOTION_HAPPY:
        return ROBOT_UI_HAPPY;
    case ROBOT_EMOTION_ALERT:
        return ROBOT_UI_ALERT;
    case ROBOT_EMOTION_CONFUSED:
        return ROBOT_UI_CONFUSED;
    case ROBOT_EMOTION_SAD:
        return ROBOT_UI_LOST;
    case ROBOT_EMOTION_NEUTRAL:
    default:
        return ROBOT_UI_IDLE;
    }
}

static robot_ui_cmd_mood_t map_command_mood(const char *emotion)
{
    robot_emotion_t parsed = robot_emotion_from_string(emotion);
    switch (parsed) {
    case ROBOT_EMOTION_HAPPY:
        return ROBOT_UI_CMD_HAPPY;
    case ROBOT_EMOTION_ALERT:
        return ROBOT_UI_CMD_ALERT;
    case ROBOT_EMOTION_CONFUSED:
    case ROBOT_EMOTION_SAD:
        return ROBOT_UI_CMD_CONFUSED;
    case ROBOT_EMOTION_NEUTRAL:
    default:
        return ROBOT_UI_CMD_NONE;
    }
}

static const char *command_text_from_status(const char *status)
{
    if (status == NULL || status[0] == '\0') {
        return "voice command";
    }

    if (status_has(status, "chassis:forward")) {
        return "moving forward";
    }
    if (status_has(status, "chassis:backward")) {
        return "backing up";
    }
    if (status_has(status, "chassis:turn_left") || status_has(status, "chassis:left")) {
        return "turning left";
    }
    if (status_has(status, "chassis:turn_right") || status_has(status, "chassis:right")) {
        return "turning right";
    }
    if (status_has(status, "chassis:stop")) {
        return "stopped";
    }
    if (status_has(status, "mode:resume_follow")) {
        return "follow enabled";
    }
    if (status_has(status, "mode:hold")) {
        return "manual hold";
    }
    if (status_has(status, "gimbal:nod")) {
        return "nodding";
    }
    if (status_has(status, "gimbal:shake")) {
        return "shaking head";
    }
    if (status_has(status, "gimbal:center")) {
        return "centering camera";
    }
    if (status_has(status, "gimbal:unlock")) {
        return "camera unlocked";
    }
    if (status_has(status, "gimbal:lock")) {
        return "camera locked";
    }
    if (status_has(status, "none:none")) {
        return "voice command";
    }
    if (status_has(status, "failed") ||
        status_has(status, "unknown") ||
        status_has(status, "invalid") ||
        status_has(status, "error")) {
        return "command failed";
    }

    return "voice command";
}

esp_err_t robot_emotion_ui_init(void)
{
    if (s_ui_ready) {
        return ESP_OK;
    }
    if (!robot_lcd_is_ready()) {
        ESP_LOGW(TAG, "LCD not ready, emotion UI disabled");
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    robot_face_ui_init(s_screen);
    lv_scr_load(s_screen);

    lvgl_port_unlock();

    s_ui_ready = true;
    ESP_LOGI(TAG, "emotion UI ready");
    return ESP_OK;
}

void robot_emotion_ui_set_status(robot_emotion_t emotion, const char *status)
{
    if (!s_ui_ready) {
        return;
    }

    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        return;
    }
    robot_face_ui_set_state(map_status_state(emotion, status));
    lvgl_port_unlock();
}

void robot_emotion_ui_set_command(const char *emotion, const char *status, uint32_t duration_ms)
{
    if (!s_ui_ready) {
        return;
    }

    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        return;
    }
    robot_face_ui_show_command(map_command_mood(emotion),
                               command_text_from_status(status),
                               duration_ms);
    lvgl_port_unlock();
}
