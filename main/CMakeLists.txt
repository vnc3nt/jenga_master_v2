idf_component_register(
    SRCS "main.cpp" "webserver.cpp" "keep_alive.cpp" "config_wifi.cpp" "udp_sync.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server esp_http_client nvs_flash esp_timer esp_netif esp_eth esp_wifi mdns
    REQUIRES driver json
    EMBED_FILES "../html/config.html"
                "../html/main.html"
               "../html/style.css"
               "../html/main.js"
               "../html/config.js"
               "../html/img/sun-moon.png" 
                    
)

