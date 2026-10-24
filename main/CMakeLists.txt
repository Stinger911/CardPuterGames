idf_component_register(
    SRCS "main.cpp" "storage.cpp" "auth.cpp" "lobby.cpp" "web.cpp" "dns.cpp" "lcd.cpp" "keyboard.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash json fatfs sdmmc driver esp_timer
                  mbedtls lwip vfs
)
