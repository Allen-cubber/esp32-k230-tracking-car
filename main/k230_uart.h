#ifndef K230_UART_H
#define K230_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define K230_UART_TEXT_MAX_LEN 192

typedef struct {
    bool valid;
    float pan_deg;
    float tilt_deg;
    float face_x;
    float face_y;
    float face_w;
    float face_h;
} k230_track_data_t;

typedef enum {
    K230_UART_PACKET_TRACK,
    K230_UART_PACKET_TEXT,
} k230_uart_packet_type_t;

typedef struct {
    k230_uart_packet_type_t type;
    k230_track_data_t track;
    char text[K230_UART_TEXT_MAX_LEN];
} k230_uart_packet_t;

esp_err_t k230_uart_init(void);
bool k230_uart_read_packet(k230_uart_packet_t *out_packet);
bool k230_uart_read_track(k230_track_data_t *out_data);
esp_err_t k230_uart_send_gimbal_command(const char *action,
                                        int32_t pan_delta_deg,
                                        int32_t tilt_delta_deg,
                                        uint32_t duration_ms);

#endif
