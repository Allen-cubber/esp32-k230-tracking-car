#pragma once

#include <stdbool.h>

typedef struct {
    bool valid;
    float pan_deg;
    float tilt_deg;
    float face_x;
    float face_y;
    float face_w;
    float face_h;
} robot_track_data_t;
