idf_component_register(SRCS "main.cpp" "ble_audio.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash ambient_ui swmic_core)
