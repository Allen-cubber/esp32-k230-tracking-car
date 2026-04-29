idf_component_register(SRCS "vehicle_ai_control.c" "deepseek_client.c" "wifi_connection.c" "station_example_main.c" "ultrasonic.c" "motor_control.c" "mpu6050.c" "k230_uart.c"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_client esp-tls json nvs_flash driver esp_timer
                       INCLUDE_DIRS ".")
