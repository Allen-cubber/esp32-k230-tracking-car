#include "robot_mqtt_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "vehicle_ai_control.h"

static const char *TAG = "robot_mqtt";

#if CONFIG_ROBOT_MQTT_ENABLE

#define ROBOT_MQTT_TOPIC_MAX_LEN 128
#define ROBOT_MQTT_RX_BUF_SIZE 1024
#define ROBOT_MQTT_ACK_CMD "execute_action"

typedef enum {
    ROBOT_MQTT_TOPIC_NONE,
    ROBOT_MQTT_TOPIC_CMD_REQUEST,
    ROBOT_MQTT_TOPIC_TRACK,
} robot_mqtt_topic_kind_t;

static esp_mqtt_client_handle_t s_client;
static char s_request_topic[ROBOT_MQTT_TOPIC_MAX_LEN];
static char s_ack_topic[ROBOT_MQTT_TOPIC_MAX_LEN];
static char s_track_topic[ROBOT_MQTT_TOPIC_MAX_LEN];
static char s_gimbal_topic[ROBOT_MQTT_TOPIC_MAX_LEN];
static char s_rx_buf[ROBOT_MQTT_RX_BUF_SIZE];
static size_t s_rx_len;
static robot_mqtt_topic_kind_t s_rx_topic_kind;
static bool s_rx_overflow;
static portMUX_TYPE s_track_lock = portMUX_INITIALIZER_UNLOCKED;
static robot_track_data_t s_latest_track;
static bool s_track_available;

static esp_err_t build_topic(char *out, size_t out_size, const char *suffix)
{
    const char *root = CONFIG_ROBOT_MQTT_TOPIC_ROOT;
    size_t root_len = strlen(root);

    while (root_len > 0 && root[root_len - 1] == '/') {
        root_len--;
    }

    int written = snprintf(out,
                           out_size,
                           "%.*s/%s/%s",
                           (int)root_len,
                           root,
                           CONFIG_ROBOT_MQTT_DEVICE_ID,
                           suffix);
    if (written < 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool topic_matches(const char *topic, int topic_len, const char *expected)
{
    size_t expected_len = strlen(expected);

    return topic != NULL &&
           topic_len == (int)expected_len &&
           strncmp(topic, expected, expected_len) == 0;
}

static const char *json_get_string(const cJSON *object,
                                   const char *key,
                                   const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return fallback;
}

static float json_get_float(const cJSON *object, const char *key, float fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return fallback;
}

static bool json_get_bool(const cJSON *object, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    return fallback;
}

static void publish_ack(const char *request_id,
                        const char *cmd,
                        bool ok,
                        const char *error)
{
    if (s_client == NULL) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to allocate ack JSON");
        return;
    }

    cJSON_AddStringToObject(root, "request_id", request_id != NULL ? request_id : "");
    cJSON_AddStringToObject(root, "cmd", cmd != NULL && cmd[0] != '\0' ? cmd : ROBOT_MQTT_ACK_CMD);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok && error != NULL && error[0] != '\0') {
        cJSON_AddStringToObject(root, "error", error);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (text == NULL) {
        ESP_LOGE(TAG, "failed to render ack JSON");
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_client, s_ack_topic, text, 0, 1, 0);
    ESP_LOGI(TAG, "ack msg_id=%d payload=%s", msg_id, text);
    cJSON_free(text);
}

static void handle_request_message(const char *message)
{
    char error[64] = {0};
    const char *request_id = "";
    const char *cmd = "";
    esp_err_t ret;

    cJSON *root = cJSON_Parse(message);
    if (root == NULL) {
        ESP_LOGW(TAG, "request JSON parse failed: %s", message);
        publish_ack("", ROBOT_MQTT_ACK_CMD, false, "json_parse_failed");
        return;
    }

    request_id = json_get_string(root, "request_id", "");
    cmd = json_get_string(root, "cmd", "");

    if (strcmp(cmd, ROBOT_MQTT_ACK_CMD) != 0) {
        ESP_LOGW(TAG, "unknown cmd: %s", cmd);
        publish_ack(request_id, cmd, false, "unknown_cmd");
        cJSON_Delete(root);
        return;
    }

    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    ret = vehicle_ai_control_execute_action_payload(payload, error, sizeof(error));
    publish_ack(request_id, cmd, ret == ESP_OK, ret == ESP_OK ? "" : error);

    cJSON_Delete(root);
}

static void store_track(const robot_track_data_t *track)
{
    taskENTER_CRITICAL(&s_track_lock);
    s_latest_track = *track;
    s_track_available = true;
    taskEXIT_CRITICAL(&s_track_lock);
}

static void handle_track_message(const char *message)
{
    cJSON *root = cJSON_Parse(message);
    if (root == NULL) {
        ESP_LOGW(TAG, "track JSON parse failed: %s", message);
        return;
    }

    robot_track_data_t track = {
        .valid = json_get_bool(root, "valid", false),
        .pan_deg = json_get_float(root, "pan", 90.0f),
        .tilt_deg = json_get_float(root, "tilt", 90.0f),
        .face_x = json_get_float(root, "x", 0.0f),
        .face_y = json_get_float(root, "y", 0.0f),
        .face_w = json_get_float(root, "w", 0.0f),
        .face_h = json_get_float(root, "h", 0.0f),
    };

    store_track(&track);
    ESP_LOGD(TAG,
             "track valid=%d pan=%.2f tilt=%.2f face=(%.1f, %.1f, %.1f, %.1f)",
             track.valid ? 1 : 0,
             track.pan_deg,
             track.tilt_deg,
             track.face_x,
             track.face_y,
             track.face_w,
             track.face_h);
    cJSON_Delete(root);
}

static robot_mqtt_topic_kind_t get_topic_kind(const char *topic, int topic_len)
{
    if (topic_matches(topic, topic_len, s_request_topic)) {
        return ROBOT_MQTT_TOPIC_CMD_REQUEST;
    }
    if (topic_matches(topic, topic_len, s_track_topic)) {
        return ROBOT_MQTT_TOPIC_TRACK;
    }
    return ROBOT_MQTT_TOPIC_NONE;
}

static bool append_topic_data(const esp_mqtt_event_handle_t event)
{
    size_t offset = (size_t)event->current_data_offset;
    size_t data_len = (size_t)event->data_len;
    size_t total_len = (size_t)event->total_data_len;

    if (event->current_data_offset == 0) {
        s_rx_len = 0;
        s_rx_overflow = false;
        s_rx_topic_kind = get_topic_kind(event->topic, event->topic_len);
    }

    if (s_rx_topic_kind == ROBOT_MQTT_TOPIC_NONE) {
        return false;
    }

    if (total_len >= sizeof(s_rx_buf) || offset + data_len >= sizeof(s_rx_buf)) {
        if (!s_rx_overflow) {
            ESP_LOGW(TAG, "payload too large: %lu bytes", (unsigned long)total_len);
            s_rx_overflow = true;
        }
        return false;
    }

    memcpy(s_rx_buf + offset, event->data, data_len);
    s_rx_len = offset + data_len;

    if (s_rx_len != total_len) {
        return false;
    }

    s_rx_buf[s_rx_len] = '\0';
    return true;
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to broker");
        int msg_id = esp_mqtt_client_subscribe(s_client, s_request_topic, 1);
        ESP_LOGI(TAG, "subscribed topic=%s msg_id=%d", s_request_topic, msg_id);
        msg_id = esp_mqtt_client_subscribe(s_client, s_track_topic, 0);
        ESP_LOGI(TAG, "subscribed topic=%s msg_id=%d", s_track_topic, msg_id);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        break;

    case MQTT_EVENT_DATA:
        if (append_topic_data(event)) {
            if (s_rx_topic_kind == ROBOT_MQTT_TOPIC_CMD_REQUEST) {
                ESP_LOGI(TAG, "request payload=%s", s_rx_buf);
                handle_request_message(s_rx_buf);
            } else if (s_rx_topic_kind == ROBOT_MQTT_TOPIC_TRACK) {
                handle_track_message(s_rx_buf);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt event error");
        break;

    default:
        ESP_LOGD(TAG, "event id=%ld", (long)event_id);
        break;
    }
}

esp_err_t robot_mqtt_client_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;
    }

    esp_err_t ret = build_topic(s_request_topic,
                                sizeof(s_request_topic),
                                "cmd/request");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "request topic too long");
        return ret;
    }

    ret = build_topic(s_ack_topic, sizeof(s_ack_topic), "cmd/ack");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ack topic too long");
        return ret;
    }

    ret = build_topic(s_track_topic, sizeof(s_track_topic), "track");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "track topic too long");
        return ret;
    }

    ret = build_topic(s_gimbal_topic, sizeof(s_gimbal_topic), "gimbal/request");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gimbal topic too long");
        return ret;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_ROBOT_MQTT_BROKER_URI,
        .session.keepalive = CONFIG_ROBOT_MQTT_KEEPALIVE_S,
    };

    if (CONFIG_ROBOT_MQTT_CLIENT_ID[0] != '\0') {
        mqtt_cfg.credentials.client_id = CONFIG_ROBOT_MQTT_CLIENT_ID;
    }
    if (CONFIG_ROBOT_MQTT_USERNAME[0] != '\0') {
        mqtt_cfg.credentials.username = CONFIG_ROBOT_MQTT_USERNAME;
        mqtt_cfg.credentials.authentication.password = CONFIG_ROBOT_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_mqtt_client_register_event(s_client,
                                         ESP_EVENT_ANY_ID,
                                         mqtt_event_handler,
                                         NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register mqtt event failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start mqtt client failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "started uri=%s request=%s ack=%s track=%s gimbal=%s",
             CONFIG_ROBOT_MQTT_BROKER_URI,
             s_request_topic,
             s_ack_topic,
             s_track_topic,
             s_gimbal_topic);
    return ESP_OK;
}

bool robot_mqtt_client_read_track(robot_track_data_t *out_track)
{
    if (out_track == NULL) {
        return false;
    }

    bool available;
    taskENTER_CRITICAL(&s_track_lock);
    available = s_track_available;
    if (available) {
        *out_track = s_latest_track;
        s_track_available = false;
    }
    taskEXIT_CRITICAL(&s_track_lock);

    return available;
}

esp_err_t robot_mqtt_client_send_gimbal_command(const char *action,
                                                int32_t pan_delta_deg,
                                                int32_t tilt_delta_deg,
                                                uint32_t duration_ms)
{
    if (s_client == NULL || action == NULL || action[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddNumberToObject(root, "pan_delta", pan_delta_deg);
    cJSON_AddNumberToObject(root, "tilt_delta", tilt_delta_deg);
    cJSON_AddNumberToObject(root, "duration_ms", duration_ms);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_client, s_gimbal_topic, text, 0, 0, 0);
    ESP_LOGI(TAG, "gimbal msg_id=%d payload=%s", msg_id, text);
    cJSON_free(text);

    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

#else

esp_err_t robot_mqtt_client_start(void)
{
    ESP_LOGI(TAG, "disabled by config");
    return ESP_OK;
}

bool robot_mqtt_client_read_track(robot_track_data_t *out_track)
{
    return false;
}

esp_err_t robot_mqtt_client_send_gimbal_command(const char *action,
                                                int32_t pan_delta_deg,
                                                int32_t tilt_delta_deg,
                                                uint32_t duration_ms)
{
    return ESP_ERR_INVALID_STATE;
}

#endif
