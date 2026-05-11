#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ROBOT_EMOTION_NEUTRAL = 0,
    ROBOT_EMOTION_HAPPY,
    ROBOT_EMOTION_ALERT,
    ROBOT_EMOTION_CONFUSED,
    ROBOT_EMOTION_SAD,
} robot_emotion_t;

esp_err_t robot_emotion_ui_init(void);
robot_emotion_t robot_emotion_from_string(const char *emotion);
void robot_emotion_ui_set_status(robot_emotion_t emotion, const char *status);
void robot_emotion_ui_set_command(const char *emotion, const char *status, uint32_t duration_ms);
