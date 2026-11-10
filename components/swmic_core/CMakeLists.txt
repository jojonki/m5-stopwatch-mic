idf_component_register(SRCS "mic.cpp" "trigger_gate.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES ambient_ui
                       PRIV_REQUIRES esp_timer)
