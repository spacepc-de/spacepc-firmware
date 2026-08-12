idf_component_register(
    SRCS "main.c" "orb_settings.c" "orb_wifi.c" "orb_net.c" "orb_geocode.c" "orb_timezone.c" "orb_route.c" "orb_map.c" "orb_data.c" "orb_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_4_3 nvs_flash esp_timer esp_wifi esp_netif
             lwip esp_http_client mbedtls json espressif__libpng
)
