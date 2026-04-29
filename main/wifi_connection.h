#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t wifi_connection_start(void);
esp_err_t wifi_connection_wait_connected(TickType_t ticks_to_wait);
bool wifi_connection_is_connected(void);
