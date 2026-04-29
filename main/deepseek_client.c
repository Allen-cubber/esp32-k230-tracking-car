#include "deepseek_client.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_connection.h"

typedef struct {
    char *data;
    int length;
    int capacity;
    bool overflow;
} deepseek_http_response_t;

static const char *TAG = "deepseek";

static esp_err_t deepseek_http_event_handler(esp_http_client_event_t *event)
{
    deepseek_http_response_t *response = (deepseek_http_response_t *)event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && response != NULL) {
        int available = response->capacity - response->length - 1;
        int copy_len = event->data_len;

        if (available <= 0) {
            response->overflow = true;
            return ESP_OK;
        }

        if (copy_len > available) {
            copy_len = available;
            response->overflow = true;
        }

        memcpy(response->data + response->length, event->data, copy_len);
        response->length += copy_len;
        response->data[response->length] = '\0';
    }

    return ESP_OK;
}

static char *deepseek_build_request_body(const char *system_text, const char *user_text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *thinking = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *system_message = cJSON_CreateObject();
    cJSON *user_message = cJSON_CreateObject();
    char *body = NULL;

    if (root == NULL || thinking == NULL || messages == NULL ||
        system_message == NULL || user_message == NULL) {
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
    cJSON_AddNumberToObject(root, "temperature", (double)CONFIG_DEEPSEEK_TEMPERATURE / 100.0);
    cJSON_AddNumberToObject(root, "max_tokens", CONFIG_DEEPSEEK_MAX_TOKENS);
    cJSON_AddFalseToObject(root, "stream");

#ifdef CONFIG_DEEPSEEK_THINKING_ENABLE
    cJSON_AddStringToObject(thinking, "type", "enabled");
    cJSON_AddStringToObject(root, "reasoning_effort", CONFIG_DEEPSEEK_REASONING_EFFORT);
#else
    cJSON_AddStringToObject(thinking, "type", "disabled");
#endif
    cJSON_AddItemToObject(root, "thinking", thinking);
    thinking = NULL;

    cJSON_AddStringToObject(system_message, "role", "system");
    cJSON_AddStringToObject(system_message, "content", system_text);
    cJSON_AddItemToArray(messages, system_message);
    system_message = NULL;

    cJSON_AddStringToObject(user_message, "role", "user");
    cJSON_AddStringToObject(user_message, "content", user_text);
    cJSON_AddItemToArray(messages, user_message);
    user_message = NULL;

    cJSON_AddItemToObject(root, "messages", messages);
    messages = NULL;

    body = cJSON_PrintUnformatted(root);

cleanup:
    cJSON_Delete(user_message);
    cJSON_Delete(system_message);
    cJSON_Delete(messages);
    cJSON_Delete(thinking);
    cJSON_Delete(root);
    return body;
}

static esp_err_t deepseek_extract_reply(const char *json, char *reply, size_t reply_size)
{
    if (reply == NULL || reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "response is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        ESP_LOGE(TAG, "DeepSeek error: %s",
                 cJSON_IsString(message) ? message->valuestring : "unknown error");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice = cJSON_GetArrayItem(choices, 0);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");

    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        ESP_LOGE(TAG, "response does not contain choices[0].message.content");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    int written = snprintf(reply, reply_size, "%s", content->valuestring);
    cJSON_Delete(root);

    if (written < 0) {
        return ESP_FAIL;
    }
    if ((size_t)written >= reply_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t deepseek_validate_config(void)
{
    if (CONFIG_DEEPSEEK_API_KEY[0] == '\0') {
        ESP_LOGE(TAG, "DeepSeek API key is empty. Set it in menuconfig first.");
        return ESP_ERR_INVALID_ARG;
    }

    if (CONFIG_DEEPSEEK_API_URL[0] == '\0' || CONFIG_DEEPSEEK_MODEL[0] == '\0') {
        ESP_LOGE(TAG, "DeepSeek API URL or model is empty.");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t deepseek_client_chat(const char *user_text, char *reply, size_t reply_size)
{
    static const char *default_system_prompt =
        "You are the language brain of an ESP32 smart car. "
        "Reply briefly and clearly. For motion commands, describe the intended car action.";

    return deepseek_client_chat_with_system(default_system_prompt, user_text, reply, reply_size);
}

esp_err_t deepseek_client_chat_with_system(const char *system_text,
                                           const char *user_text,
                                           char *reply,
                                           size_t reply_size)
{
    if (user_text == NULL || user_text[0] == '\0' || reply == NULL || reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (system_text == NULL || system_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = deepseek_validate_config();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!wifi_connection_is_connected()) {
        ret = wifi_connection_wait_connected(pdMS_TO_TICKS(CONFIG_DEEPSEEK_WIFI_WAIT_TIMEOUT_MS));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi is not connected: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    char *request_body = deepseek_build_request_body(system_text, user_text);
    if (request_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int auth_header_len = strlen("Bearer ") + strlen(CONFIG_DEEPSEEK_API_KEY) + 1;
    char *auth_header = malloc(auth_header_len);
    char *response_buffer = calloc(1, CONFIG_DEEPSEEK_MAX_RESPONSE_BYTES);
    if (auth_header == NULL || response_buffer == NULL) {
        free(request_body);
        free(auth_header);
        free(response_buffer);
        return ESP_ERR_NO_MEM;
    }

    snprintf(auth_header, auth_header_len, "Bearer %s", CONFIG_DEEPSEEK_API_KEY);

    deepseek_http_response_t response = {
        .data = response_buffer,
        .length = 0,
        .capacity = CONFIG_DEEPSEEK_MAX_RESPONSE_BYTES,
        .overflow = false,
    };

    esp_http_client_config_t config = {
        .url = CONFIG_DEEPSEEK_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_DEEPSEEK_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = deepseek_http_event_handler,
        .user_data = &response,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(request_body);
        free(auth_header);
        free(response_buffer);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Authorization", auth_header));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client,
                                                                request_body,
                                                                strlen(request_body)));

    ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
    } else if (response.overflow) {
        ESP_LOGE(TAG, "response exceeded %d bytes", CONFIG_DEEPSEEK_MAX_RESPONSE_BYTES);
        ret = ESP_ERR_NO_MEM;
    } else if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "HTTP status %d: %s", status_code, response.data);
        ret = ESP_FAIL;
    } else {
        ret = deepseek_extract_reply(response.data, reply, reply_size);
    }

    esp_http_client_cleanup(client);
    free(request_body);
    free(auth_header);
    free(response_buffer);

    return ret;
}

#ifdef CONFIG_DEEPSEEK_BOOT_TEST_ENABLE
static void deepseek_boot_test_task(void *arg)
{
    char reply[CONFIG_DEEPSEEK_BOOT_TEST_REPLY_BYTES] = {0};

    esp_err_t ret = deepseek_client_chat(CONFIG_DEEPSEEK_BOOT_TEST_PROMPT,
                                         reply,
                                         sizeof(reply));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "boot test reply: %s", reply);
    } else {
        ESP_LOGE(TAG, "boot test failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}
#endif

void deepseek_client_start_boot_test(void)
{
#ifdef CONFIG_DEEPSEEK_BOOT_TEST_ENABLE
    BaseType_t created = xTaskCreate(deepseek_boot_test_task,
                                     "deepseek_test",
                                     CONFIG_DEEPSEEK_BOOT_TEST_STACK_SIZE,
                                     NULL,
                                     5,
                                     NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create boot test task");
    }
#endif
}
