#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t deepseek_client_chat(const char *user_text, char *reply, size_t reply_size);
esp_err_t deepseek_client_chat_with_system(const char *system_text,
                                           const char *user_text,
                                           char *reply,
                                           size_t reply_size);
void deepseek_client_start_boot_test(void);
