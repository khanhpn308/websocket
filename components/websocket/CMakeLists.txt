idf_component_register(SRCS "websocket.c"
                       PRIV_REQUIRES esp_system esp_timer log esp_websocket_client proto
                       INCLUDE_DIRS ".")