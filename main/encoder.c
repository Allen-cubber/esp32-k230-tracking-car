#include "encoder.h"

#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "encoder";

#define ENCODER_PCNT_HIGH_LIMIT 32767
#define ENCODER_PCNT_LOW_LIMIT  -32768
#define ENCODER_GLITCH_NS       1000

typedef struct {
    const char *name;
    int gpio_a;
    int gpio_b;
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t chan_a;
    pcnt_channel_handle_t chan_b;
} encoder_wheel_t;

static encoder_wheel_t s_left = {
    .name = "left",
    .gpio_a = ENCODER_L_A_PIN,
    .gpio_b = ENCODER_L_B_PIN,
};

static encoder_wheel_t s_right = {
    .name = "right",
    .gpio_a = ENCODER_R_A_PIN,
    .gpio_b = ENCODER_R_B_PIN,
};

static bool s_encoder_ready;

static esp_err_t encoder_setup_wheel(encoder_wheel_t *wheel)
{
    pcnt_unit_config_t unit_config = {
        .high_limit = ENCODER_PCNT_HIGH_LIMIT,
        .low_limit = ENCODER_PCNT_LOW_LIMIT,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &wheel->unit), TAG,
                        "create %s pcnt unit failed", wheel->name);

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = ENCODER_GLITCH_NS,
    };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(wheel->unit, &filter_config), TAG,
                        "set %s glitch filter failed", wheel->name);

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = wheel->gpio_a,
        .level_gpio_num = wheel->gpio_b,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(wheel->unit, &chan_a_config, &wheel->chan_a), TAG,
                        "create %s channel A failed", wheel->name);

    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = wheel->gpio_b,
        .level_gpio_num = wheel->gpio_a,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(wheel->unit, &chan_b_config, &wheel->chan_b), TAG,
                        "create %s channel B failed", wheel->name);

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(wheel->chan_a,
                                                    PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                    PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                        TAG, "set %s channel A edge action failed", wheel->name);
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(wheel->chan_a,
                                                     PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                     PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                        TAG, "set %s channel A level action failed", wheel->name);

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(wheel->chan_b,
                                                    PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                    PCNT_CHANNEL_EDGE_ACTION_DECREASE),
                        TAG, "set %s channel B edge action failed", wheel->name);
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(wheel->chan_b,
                                                     PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                     PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                        TAG, "set %s channel B level action failed", wheel->name);

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(wheel->unit), TAG, "enable %s unit failed", wheel->name);
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(wheel->unit), TAG, "clear %s count failed", wheel->name);
    ESP_RETURN_ON_ERROR(pcnt_unit_start(wheel->unit), TAG, "start %s unit failed", wheel->name);

    ESP_LOGI(TAG, "%s encoder ready A=%d B=%d", wheel->name, wheel->gpio_a, wheel->gpio_b);
    return ESP_OK;
}

esp_err_t encoder_init(void)
{
    if (s_encoder_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(encoder_setup_wheel(&s_left), TAG, "left encoder init failed");
    ESP_RETURN_ON_ERROR(encoder_setup_wheel(&s_right), TAG, "right encoder init failed");

    s_encoder_ready = true;
    ESP_LOGI(TAG, "quadrature encoders initialized");
    return ESP_OK;
}

bool encoder_is_ready(void)
{
    return s_encoder_ready;
}

esp_err_t encoder_read_and_clear(int32_t *left_delta, int32_t *right_delta)
{
    if (!s_encoder_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!left_delta || !right_delta) {
        return ESP_ERR_INVALID_ARG;
    }

    int left = 0;
    int right = 0;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(s_left.unit, &left), TAG, "read left count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(s_right.unit, &right), TAG, "read right count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_left.unit), TAG, "clear left count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_right.unit), TAG, "clear right count failed");

    *left_delta = (int32_t)left;
    *right_delta = (int32_t)right;
    return ESP_OK;
}

esp_err_t encoder_get_counts(int32_t *left_count, int32_t *right_count)
{
    if (!s_encoder_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!left_count || !right_count) {
        return ESP_ERR_INVALID_ARG;
    }

    int left = 0;
    int right = 0;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(s_left.unit, &left), TAG, "read left count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(s_right.unit, &right), TAG, "read right count failed");

    *left_count = (int32_t)left;
    *right_count = (int32_t)right;
    return ESP_OK;
}
