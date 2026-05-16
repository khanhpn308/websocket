idf_component_register(SRCS "mqtt.c"
                       REQUIRES websocket
                       PRIV_REQUIRES mqtt proto
                       INCLUDE_DIRS ".")