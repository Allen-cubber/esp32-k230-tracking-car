#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Encoder pin assignment for ESP32-S3 N16R8 module.
// Avoid GPIO35/36/37/38 because they are reserved by the module's SPI flash/PSRAM path.
#define ENCODER_L_A_PIN 39
#define ENCODER_L_B_PIN 40
#define ENCODER_R_A_PIN 41
#define ENCODER_R_B_PIN 42

esp_err_t encoder_init(void);
bool encoder_is_ready(void);

// Read signed quadrature count deltas since the previous call and clear hardware counters.
// The motor controller uses the absolute delta for speed, so A/B phase direction does not
// need to be perfect for the first closed-loop bring-up.
esp_err_t encoder_read_and_clear(int32_t *left_delta, int32_t *right_delta);

// Read current raw hardware counters without clearing them.
esp_err_t encoder_get_counts(int32_t *left_count, int32_t *right_count);
