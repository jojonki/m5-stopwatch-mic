idf_component_register(SRCS "main.cpp" "usb_descriptors.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES usb ambient_ui swmic_core
                       WHOLE_ARCHIVE)

# TinyUSB looks for tusb_config.h on its include path; provide ours.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
