#include "robot_lcd.h"

#include "sdkconfig.h"

#if CONFIG_ROBOT_LCD_ENABLE

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8
#define LCD_BITS_PER_PIXEL 16
#define LCD_DRAW_LINES     24

static const char *TAG = "robot_lcd";

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd_panel;
static lv_display_t *s_lcd_display;
static bool s_lcd_ready;

static spi_host_device_t lcd_spi_host(void)
{
    return (CONFIG_ROBOT_LCD_SPI_HOST == 3) ? SPI3_HOST : SPI2_HOST;
}

static esp_err_t lcd_backlight_init(void)
{
    if (CONFIG_ROBOT_LCD_BL_GPIO < 0) {
        return ESP_OK;
    }

    const gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << CONFIG_ROBOT_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "backlight gpio config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(CONFIG_ROBOT_LCD_BL_GPIO, !CONFIG_ROBOT_LCD_BL_ON_LEVEL),
                        TAG,
                        "backlight off failed");
    return ESP_OK;
}

static esp_err_t lcd_backlight_on(void)
{
    if (CONFIG_ROBOT_LCD_BL_GPIO < 0) {
        return ESP_OK;
    }

    return gpio_set_level(CONFIG_ROBOT_LCD_BL_GPIO, CONFIG_ROBOT_LCD_BL_ON_LEVEL);
}

static esp_err_t lcd_spi_bus_init(void)
{
    const int buffer_pixels = CONFIG_ROBOT_LCD_H_RES * LCD_DRAW_LINES;
    const spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_ROBOT_LCD_SCLK_GPIO,
        .mosi_io_num = CONFIG_ROBOT_LCD_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = buffer_pixels * sizeof(uint16_t),
    };

    esp_err_t ret = spi_bus_initialize(lcd_spi_host(), &buscfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized");
        return ESP_OK;
    }
    return ret;
}

static void lcd_cleanup_after_error(void)
{
    if (s_lcd_panel != NULL) {
        esp_lcd_panel_del(s_lcd_panel);
        s_lcd_panel = NULL;
    }
    if (s_lcd_io != NULL) {
        esp_lcd_panel_io_del(s_lcd_io);
        s_lcd_io = NULL;
    }
    spi_bus_free(lcd_spi_host());
}

esp_err_t robot_lcd_init(void)
{
    if (s_lcd_ready) {
        return ESP_OK;
    }

    esp_err_t ret;
    ESP_LOGI(TAG,
             "init ST7735 LCD sclk=%d mosi=%d cs=%d dc=%d rst=%d bl=%d %dx%d",
             CONFIG_ROBOT_LCD_SCLK_GPIO,
             CONFIG_ROBOT_LCD_MOSI_GPIO,
             CONFIG_ROBOT_LCD_CS_GPIO,
             CONFIG_ROBOT_LCD_DC_GPIO,
             CONFIG_ROBOT_LCD_RST_GPIO,
             CONFIG_ROBOT_LCD_BL_GPIO,
             CONFIG_ROBOT_LCD_H_RES,
             CONFIG_ROBOT_LCD_V_RES);

    ESP_GOTO_ON_ERROR(lcd_backlight_init(), err, TAG, "backlight init failed");
    ESP_GOTO_ON_ERROR(lcd_spi_bus_init(), err, TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_ROBOT_LCD_DC_GPIO,
        .cs_gpio_num = CONFIG_ROBOT_LCD_CS_GPIO,
        .pclk_hz = CONFIG_ROBOT_LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)lcd_spi_host(),
                                               &io_config,
                                               &s_lcd_io),
                      err,
                      TAG,
                      "panel IO init failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_ROBOT_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7735(s_lcd_io, &panel_config, &s_lcd_panel),
                      err,
                      TAG,
                      "ST7735 driver init failed");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), err, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), err, TAG, "panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(s_lcd_panel,
                                            CONFIG_ROBOT_LCD_X_GAP,
                                            CONFIG_ROBOT_LCD_Y_GAP),
                      err,
                      TAG,
                      "panel set gap failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel,
                                                 CONFIG_ROBOT_LCD_INVERT_COLOR),
                      err,
                      TAG,
                      "panel invert failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true),
                      err,
                      TAG,
                      "panel display on failed");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_GOTO_ON_ERROR(lvgl_port_init(&lvgl_cfg), err, TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = CONFIG_ROBOT_LCD_H_RES * LCD_DRAW_LINES,
        .double_buffer = true,
        .hres = CONFIG_ROBOT_LCD_H_RES,
        .vres = CONFIG_ROBOT_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    s_lcd_display = lvgl_port_add_disp(&disp_cfg);
    ESP_GOTO_ON_FALSE(s_lcd_display != NULL, ESP_FAIL, err, TAG, "LVGL display add failed");

    ESP_GOTO_ON_ERROR(lcd_backlight_on(), err, TAG, "backlight on failed");

    s_lcd_ready = true;
    ESP_LOGI(TAG, "ST7735 LVGL display ready");
    return ESP_OK;

err:
    lcd_cleanup_after_error();
    ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(ret));
    return ret;
}

bool robot_lcd_is_ready(void)
{
    return s_lcd_ready;
}

lv_disp_t *robot_lcd_get_display(void)
{
    return s_lcd_display;
}

#else

esp_err_t robot_lcd_init(void)
{
    return ESP_OK;
}

bool robot_lcd_is_ready(void)
{
    return false;
}

lv_disp_t *robot_lcd_get_display(void)
{
    return NULL;
}

#endif
