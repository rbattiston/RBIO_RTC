idf_component_register(
    SRCS
        "example_main.c"
        "rbio_rtc_client.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_timer
        mbedtls
)
