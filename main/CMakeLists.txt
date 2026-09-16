# idf_component_register(SRCS "main.c"
#                     INCLUDE_DIRS ".")

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES "esp_tinyusb")

# idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
# target_include_directories(${tusb_lib} PRIVATE "hid_device")