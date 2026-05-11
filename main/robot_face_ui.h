#pragma once

#include <stdint.h>

#include "lvgl.h"

typedef enum {
    ROBOT_UI_BOOT = 0,
    ROBOT_UI_IDLE,
    ROBOT_UI_FOLLOW,
    ROBOT_UI_HAPPY,
    ROBOT_UI_LOST,
    ROBOT_UI_WAIT_TRACK,
    ROBOT_UI_SEARCH,
    ROBOT_UI_OBSTACLE,
    ROBOT_UI_TOO_CLOSE,
    ROBOT_UI_TILT_LIMIT,
    ROBOT_UI_ALERT,
    ROBOT_UI_HOLD,
    ROBOT_UI_CONFUSED,
} robot_ui_state_t;

typedef enum {
    ROBOT_UI_CMD_NONE = 0,
    ROBOT_UI_CMD_HAPPY,
    ROBOT_UI_CMD_ALERT,
    ROBOT_UI_CMD_CONFUSED,
} robot_ui_cmd_mood_t;

void robot_face_ui_init(lv_obj_t *parent);
void robot_face_ui_set_state(robot_ui_state_t state);
void robot_face_ui_show_command(robot_ui_cmd_mood_t mood,
                                const char *command_text,
                                uint32_t duration_ms);
void robot_face_ui_tick(void);
