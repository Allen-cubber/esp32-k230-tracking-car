#include "wifi_connection.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define WIFI_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define WIFI_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define WIFI_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;
static bool s_started;
static bool s_connected;
static int s_retry_num;

static esp_err_t return_on_error(esp_err_t err, const char *action)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", action, esp_err_to_name(err));
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_retry_num = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        s_retry_num++;
        esp_wifi_connect();

        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)",
                     s_retry_num, CONFIG_ESP_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "Wi-Fi still disconnected after %d retries, background retry continues",
                     s_retry_num);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_retry_num = 0;
        s_connected = true;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_connection_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        return return_on_error(ret, "esp_netif_init");
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return return_on_error(ret, "esp_event_loop_create_default");
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        return return_on_error(ret, "esp_wifi_init");
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        return return_on_error(ret, "register Wi-Fi event handler");
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        return return_on_error(ret, "register IP event handler");
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = WIFI_H2E_IDENTIFIER,
        },
    };
    strncpy((char *)wifi_config.sta.ssid,
            CONFIG_ESP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password,
            CONFIG_ESP_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return return_on_error(ret, "esp_wifi_set_mode");
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        return return_on_error(ret, "esp_wifi_set_config");
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return return_on_error(ret, "esp_wifi_start");
    }

    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started, connecting to SSID: %s", CONFIG_ESP_WIFI_SSID);

    return ESP_OK;
}

esp_err_t wifi_connection_wait_connected(TickType_t ticks_to_wait)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           ticks_to_wait);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

bool wifi_connection_is_connected(void)
{
    return s_connected;
}
