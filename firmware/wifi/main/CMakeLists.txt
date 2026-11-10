idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_event lwip esp_timer ambient_ui swmic_core)
