#include "ultrasonic.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdint.h>
#include "esp_timer.h" // 🌟 新增：包含标准高精度定时器头文件

// 包含时间函数头文件 (主要为了保留 ets_delay_us)
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/ets_sys.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#include "esp32s2/rom/ets_sys.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/ets_sys.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#include "esp32c3/rom/ets_sys.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#include "esp32c6/rom/ets_sys.h"
#else
#include "rom/ets_sys.h"
#endif

static const char* TAG = "ultrasonic";

// 微秒延迟函数
static void delay_us(uint32_t us) {
    // 如果之后版本连 ets_delay_us 也报错，可以替换为 esp_rom_delay_us(us);
    ets_delay_us(us); 
}

// 获取当前时间（微秒）
static int64_t get_time_us(void) {
    // 🌟 修改：使用 ESP-IDF 标准的高精度时间 API
    return esp_timer_get_time(); 
}

void ultrasonic_init(void) {
    // 配置 Trig 引脚为输出
    gpio_config_t trig_conf = {
        .pin_bit_mask = (1ULL << ULTRASONIC_TRIG_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&trig_conf);
    gpio_set_level(ULTRASONIC_TRIG_PIN, 0);

    // 配置 Echo 引脚为输入
    gpio_config_t echo_conf = {
        .pin_bit_mask = (1ULL << ULTRASONIC_ECHO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&echo_conf);

    ESP_LOGI(TAG, "Ultrasonic sensor initialized (Trig=%d, Echo=%d)",
             ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN);
}

float ultrasonic_measure_cm(void) {
    // 发送10us高电平触发脉冲
    gpio_set_level(ULTRASONIC_TRIG_PIN, 1);
    delay_us(10);
    gpio_set_level(ULTRASONIC_TRIG_PIN, 0);

    // 等待 Echo 引脚变高 (上升沿)
    int64_t start_time = get_time_us();
    while (gpio_get_level(ULTRASONIC_ECHO_PIN) == 0) {
        if (get_time_us() - start_time > TIMEOUT_US) {
            ESP_LOGW(TAG, "Timeout waiting for echo start");
            return -1.0f;
        }
    }
    int64_t echo_start = get_time_us();

    // 等待 Echo 引脚变低 (下降沿)
    while (gpio_get_level(ULTRASONIC_ECHO_PIN) == 1) {
        if (get_time_us() - echo_start > TIMEOUT_US) {
            ESP_LOGW(TAG, "Timeout waiting for echo end");
            return -1.0f;
        }
    }
    int64_t echo_end = get_time_us();

    // 计算高电平持续时间 (微秒)
    int64_t duration_us = echo_end - echo_start;

    // 计算距离: 时间(us) * 声速(cm/us) / 2 (往返距离)
    float distance_cm = (float)duration_us * SOUND_SPEED_CM_US / 2.0f;

    // 如果距离超出范围，返回 -1
    if (distance_cm > MAX_DISTANCE_CM || distance_cm < 0) {
        ESP_LOGW(TAG, "Distance out of range: %.2f cm", distance_cm);
        return -1.0f;
    }

    ESP_LOGD(TAG, "Measured distance: %.2f cm", distance_cm);
    return distance_cm;
}
