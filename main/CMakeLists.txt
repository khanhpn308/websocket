idf_component_register(SRCS "coordinates_data.pb.c" "station_example_main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash nanopb random
                    INCLUDE_DIRS ".")
