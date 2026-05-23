idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ""
                    REQUIRES nvs_flash esp_http_client esp-tls driver esp_wifi freertos esp_event esp_http_server fatfs sdmmc json)
