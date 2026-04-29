#include "k230_uart.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"

#define K230_UART_PORT        UART_NUM_1
#define K230_UART_BUF_SIZE    256
#define K230_LINE_BUF_SIZE    256
#define K230_TX_LINE_BUF_SIZE 96

static const char *TAG = "k230_uart";
static char s_line_buf[K230_LINE_BUF_SIZE];
static size_t s_line_len = 0;

esp_err_t k230_uart_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = CONFIG_K230_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(K230_UART_PORT, K230_UART_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(K230_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(K230_UART_PORT,
                                 CONFIG_K230_UART_TX_GPIO,
                                 CONFIG_K230_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d ready, RX=%d TX=%d baud=%d",
             K230_UART_PORT,
             CONFIG_K230_UART_RX_GPIO,
             CONFIG_K230_UART_TX_GPIO,
             CONFIG_K230_UART_BAUDRATE);
    return ESP_OK;
}

static bool k230_parse_track_line(const char *line, k230_track_data_t *out_data)
{
    const char *track_start;
    int valid = 0;

    track_start = strstr(line, "TRACK,");
    if (track_start == NULL) {
        return false;
    }

    int parsed = sscanf(track_start, "TRACK,%d,%f,%f,%f,%f,%f,%f",
                        &valid,
                        &out_data->pan_deg,
                        &out_data->tilt_deg,
                        &out_data->face_x,
                        &out_data->face_y,
                        &out_data->face_w,
                        &out_data->face_h);
    if (parsed == 7) {
        out_data->valid = (valid != 0);
        return true;
    }
    return false;
}

static bool k230_parse_text_line(const char *line, char *out_text, size_t out_text_size)
{
    const char *text_start = strstr(line, "TEXT,");

    if (text_start == NULL || out_text == NULL || out_text_size == 0) {
        return false;
    }

    text_start += strlen("TEXT,");
    if (text_start[0] == '\0') {
        return false;
    }

    snprintf(out_text, out_text_size, "%s", text_start);
    return true;
}

static bool k230_parse_packet_line(const char *line, k230_uart_packet_t *out_packet)
{
    k230_track_data_t track = {0};

    if (k230_parse_track_line(line, &track)) {
        out_packet->type = K230_UART_PACKET_TRACK;
        out_packet->track = track;
        return true;
    }

    if (k230_parse_text_line(line, out_packet->text, sizeof(out_packet->text))) {
        out_packet->type = K230_UART_PACKET_TEXT;
        return true;
    }

    return false;
}

bool k230_uart_read_packet(k230_uart_packet_t *out_packet)
{
    uint8_t ch;
    int bytes_read;

    if (out_packet == NULL) {
        return false;
    }

    while ((bytes_read = uart_read_bytes(K230_UART_PORT, &ch, 1, 0)) > 0) {
        char c = (char)ch;

        if (c == '\r') {
            continue;
        }

        if (c == '#') {
            s_line_len = 0;
            continue;
        }

        if (c == '\n') {
            k230_uart_packet_t parsed = {0};
            size_t parsed_line_len = s_line_len;

            s_line_buf[s_line_len] = '\0';
            s_line_len = 0;

            if (k230_parse_packet_line(s_line_buf, &parsed)) {
                *out_packet = parsed;
                return true;
            }

            if (parsed_line_len > 0) {
                ESP_LOGD(TAG, "ignored non-track line: %s", s_line_buf);
            }
            continue;
        }

        if ((s_line_len == 0) && (c != 'T') && (c != 'R') && (c != 'A') &&
            (c != 'C') && (c != 'K') && (c != 'E') && (c != 'X') &&
            (c != ',') && (c != '-') && (c != '.') && ((c < '0') || (c > '9'))) {
            continue;
        }

        if (s_line_len < (K230_LINE_BUF_SIZE - 1)) {
            s_line_buf[s_line_len++] = c;
        } else {
            s_line_len = 0;
            ESP_LOGW(TAG, "line too long, dropped");
        }
    }

    return false;
}

bool k230_uart_read_track(k230_track_data_t *out_data)
{
    k230_uart_packet_t packet = {0};

    if (out_data == NULL) {
        return false;
    }

    if (!k230_uart_read_packet(&packet)) {
        return false;
    }

    if (packet.type != K230_UART_PACKET_TRACK) {
        return false;
    }

    *out_data = packet.track;
    return true;
}

esp_err_t k230_uart_send_gimbal_command(const char *action,
                                        int32_t pan_delta_deg,
                                        int32_t tilt_delta_deg,
                                        uint32_t duration_ms)
{
    char line[K230_TX_LINE_BUF_SIZE];

    if (action == NULL || action[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int len = snprintf(line,
                       sizeof(line),
                       "GIMBAL,%s,%ld,%ld,%lu\n",
                       action,
                       (long)pan_delta_deg,
                       (long)tilt_delta_deg,
                       (unsigned long)duration_ms);
    if (len < 0 || len >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written = uart_write_bytes(K230_UART_PORT, line, len);
    if (written != len) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sent %s", line);
    return ESP_OK;
}
