#include "mpu6050.h"

#include <math.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_MPU6050_GYRO_DEADBAND_DPS
#define CONFIG_MPU6050_GYRO_DEADBAND_DPS 20
#endif

#ifndef CONFIG_MPU6050_BIAS_LEARN_DPS
#define CONFIG_MPU6050_BIAS_LEARN_DPS 30
#endif

#ifndef CONFIG_MPU6050_BIAS_LEARN_ALPHA
#define CONFIG_MPU6050_BIAS_LEARN_ALPHA 10
#endif

#define MPU6050_I2C_PORT         I2C_NUM_0
#define MPU6050_I2C_FREQ_HZ      400000
#define MPU6050_I2C_TIMEOUT_MS   100
#define MPU6050_I2C_ADDRESS      0x68

#define MPU6050_REG_SMPLRT_DIV   0x19
#define MPU6050_REG_CONFIG       0x1A
#define MPU6050_REG_GYRO_CONFIG  0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_INT_ENABLE   0x38
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_WHO_AM_I     0x75

#define MPU6050_WHO_AM_I_VALUE   0x68
#define MPU6050_ACCEL_LSB_PER_G  16384.0f
#define MPU6050_GYRO_LSB_PER_DPS 131.0f
#define MPU6050_GYRO_DEADBAND_DPS   (CONFIG_MPU6050_GYRO_DEADBAND_DPS / 10.0f)
#define MPU6050_BIAS_LEARN_DPS      (CONFIG_MPU6050_BIAS_LEARN_DPS / 10.0f)
#define MPU6050_BIAS_LEARN_ALPHA    (CONFIG_MPU6050_BIAS_LEARN_ALPHA / 1000.0f)

static const char *TAG = "mpu6050";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_mpu_dev = NULL;
static bool s_initialized = false;
static bool s_calibrated = false;
static float s_gyro_z_bias_dps = 0.0f;
static float s_yaw_deg = 0.0f;

static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(s_mpu_dev, buffer, sizeof(buffer), MPU6050_I2C_TIMEOUT_MS);
}

static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_mpu_dev, &reg, 1, data, len, MPU6050_I2C_TIMEOUT_MS);
}

static int16_t read_be_i16(const uint8_t *buf)
{
    return (int16_t)((buf[0] << 8) | buf[1]);
}

esp_err_t mpu6050_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = MPU6050_I2C_PORT,
        .sda_io_num = CONFIG_MPU6050_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_MPU6050_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "create i2c bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6050_I2C_ADDRESS,
        .scl_speed_hz = MPU6050_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_mpu_dev), TAG, "add device failed");

    uint8_t who_am_i = 0;
    ESP_RETURN_ON_ERROR(mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &who_am_i, 1), TAG, "who_am_i read failed");
    ESP_RETURN_ON_FALSE(who_am_i == MPU6050_WHO_AM_I_VALUE, ESP_ERR_NOT_FOUND, TAG, "unexpected who_am_i: 0x%02x", who_am_i);

    ESP_RETURN_ON_ERROR(mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00), TAG, "wake chip failed");
    ESP_RETURN_ON_ERROR(mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, 0x07), TAG, "sample rate set failed");
    ESP_RETURN_ON_ERROR(mpu6050_write_reg(MPU6050_REG_CONFIG, 0x03), TAG, "dlpf set failed");
    ESP_RETURN_ON_ERROR(mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, 0x00), TAG, "gyro range set failed");
    ESP_RETURN_ON_ERROR(mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, 0x00), TAG, "accel range set failed");
    ESP_RETURN_ON_ERROR(mpu6050_write_reg(MPU6050_REG_INT_ENABLE, 0x00), TAG, "int config failed");

    s_initialized = true;
    s_calibrated = false;
    s_gyro_z_bias_dps = 0.0f;
    s_yaw_deg = 0.0f;

    ESP_LOGI(TAG, "initialized on SDA=%d SCL=%d addr=0x%02X",
             CONFIG_MPU6050_I2C_SDA_GPIO,
             CONFIG_MPU6050_I2C_SCL_GPIO,
             MPU6050_I2C_ADDRESS);
    return ESP_OK;
}

esp_err_t mpu6050_read(mpu6050_data_t *data)
{
    uint8_t raw[14];
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "device not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is null");
    ESP_RETURN_ON_ERROR(mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw)), TAG, "sensor read failed");

    data->accel_x_g = read_be_i16(&raw[0]) / MPU6050_ACCEL_LSB_PER_G;
    data->accel_y_g = read_be_i16(&raw[2]) / MPU6050_ACCEL_LSB_PER_G;
    data->accel_z_g = read_be_i16(&raw[4]) / MPU6050_ACCEL_LSB_PER_G;
    data->temperature_c = (read_be_i16(&raw[6]) / 340.0f) + 36.53f;
    data->gyro_x_dps = read_be_i16(&raw[8]) / MPU6050_GYRO_LSB_PER_DPS;
    data->gyro_y_dps = read_be_i16(&raw[10]) / MPU6050_GYRO_LSB_PER_DPS;
    data->gyro_z_dps = read_be_i16(&raw[12]) / MPU6050_GYRO_LSB_PER_DPS;

    return ESP_OK;
}

esp_err_t mpu6050_calibrate_gyro(size_t sample_count)
{
    mpu6050_data_t sample;
    float sum_z = 0.0f;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "device not initialized");
    ESP_RETURN_ON_FALSE(sample_count > 0, ESP_ERR_INVALID_ARG, TAG, "sample_count must be > 0");

    ESP_LOGI(TAG, "calibrating gyro, keep the car still...");
    for (size_t i = 0; i < sample_count; ++i) {
        ESP_RETURN_ON_ERROR(mpu6050_read(&sample), TAG, "calibration read failed");
        sum_z += sample.gyro_z_dps;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    s_gyro_z_bias_dps = sum_z / (float)sample_count;
    s_yaw_deg = 0.0f;
    s_calibrated = true;
    ESP_LOGI(TAG, "gyro z bias = %.3f dps", s_gyro_z_bias_dps);
    return ESP_OK;
}

esp_err_t mpu6050_update_pose(mpu6050_pose_t *pose, float dt_s)
{
    mpu6050_data_t data;
    float raw_gyro_z_dps;
    float yaw_rate_dps;

    ESP_RETURN_ON_FALSE(pose != NULL, ESP_ERR_INVALID_ARG, TAG, "pose is null");
    ESP_RETURN_ON_FALSE(dt_s > 0.0f, ESP_ERR_INVALID_ARG, TAG, "dt_s must be > 0");
    ESP_RETURN_ON_ERROR(mpu6050_read(&data), TAG, "pose read failed");

    raw_gyro_z_dps = data.gyro_z_dps;

    if (fabsf(raw_gyro_z_dps - s_gyro_z_bias_dps) < MPU6050_BIAS_LEARN_DPS) {
        s_gyro_z_bias_dps =
            ((1.0f - MPU6050_BIAS_LEARN_ALPHA) * s_gyro_z_bias_dps) +
            (MPU6050_BIAS_LEARN_ALPHA * raw_gyro_z_dps);
    }

    yaw_rate_dps = raw_gyro_z_dps - s_gyro_z_bias_dps;
    if (fabsf(yaw_rate_dps) < MPU6050_GYRO_DEADBAND_DPS) {
        yaw_rate_dps = 0.0f;
    }

    s_yaw_deg += yaw_rate_dps * dt_s;

    pose->yaw_deg = s_yaw_deg;
    pose->yaw_rate_dps = yaw_rate_dps;
    pose->calibrated = s_calibrated;
    return ESP_OK;
}

void mpu6050_reset_yaw(float yaw_deg)
{
    s_yaw_deg = yaw_deg;
}
