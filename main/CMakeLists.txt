idf_component_register(SRCS "robot_face_ui.c" "robot_emotion_ui.c" "robot_lcd.c" "robot_mqtt_client.c" "vehicle_ai_control.c" "deepseek_client.c" "wifi_connection.c" "station_example_main.c" "ultrasonic.c" "motor_control.c" "mpu6050.c"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_client esp-tls json mqtt nvs_flash driver esp_timer esp_lcd espressif__esp_lvgl_port lvgl__lvgl waveshare__esp_lcd_st7735
                       INCLUDE_DIRS ".")
