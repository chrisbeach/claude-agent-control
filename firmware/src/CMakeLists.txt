idf_component_register(
    SRCS "main.c" "leds.c" "board.c" "wifi.c" "fetch.c" "ota.c" "lv_mem_psram.c"
         "../shared/fleet.c" "../shared/ui.c"
    INCLUDE_DIRS "." "../shared"
    EMBED_FILES "../shared/fonts/Antonio-Bold.ttf"
    PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_wifi esp_netif nvs_flash
                  esp_http_client json esp_timer app_update esp_app_format esp_http_server mbedtls esp_driver_rmt
)
