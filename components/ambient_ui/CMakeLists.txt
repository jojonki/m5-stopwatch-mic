idf_component_register(SRCS "ui.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_psram)

# M5GFX includes esp_psram.h but does not declare the dependency.
idf_component_get_property(gfx_lib m5stack__m5gfx COMPONENT_LIB)
target_link_libraries(${gfx_lib} PRIVATE idf::esp_psram)
