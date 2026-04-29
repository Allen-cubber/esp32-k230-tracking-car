#ifndef MPU6050_H
#define MPU6050_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float temperature_c;
} mpu6050_data_t;

typedef struct {
    float yaw_deg;
    float yaw_rate_dps;
    bool calibrated;
} mpu6050_pose_t;

esp_err_t mpu6050_init(void);
esp_err_t mpu6050_calibrate_gyro(size_t sample_count);
esp_err_t mpu6050_read(mpu6050_data_t *data);
esp_err_t mpu6050_update_pose(mpu6050_pose_t *pose, float dt_s);
void mpu6050_reset_yaw(float yaw_deg);

#endif
