#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

esp_err_t robot_lcd_init(void);
bool robot_lcd_is_ready(void);
lv_disp_t *robot_lcd_get_display(void);
