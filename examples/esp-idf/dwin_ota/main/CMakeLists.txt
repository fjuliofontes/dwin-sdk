idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES dwin esp_wifi nvs_flash esp_netif esp_event)
