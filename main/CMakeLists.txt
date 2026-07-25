idf_component_register(SRCS "main.c" "crypto_bench.c" "clients.c" "status.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_timer esp_driver_gpio esp_driver_tsens esp_http_server protocol_examples_common wireguard mbedtls espressif__libsodium espressif__mdns)
