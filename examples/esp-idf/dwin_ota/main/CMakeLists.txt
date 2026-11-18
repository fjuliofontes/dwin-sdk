# An ESP-IDF component is named after its folder. In a project the SDK sits in components/dwin
# and is required as "dwin"; here it is the checkout that contains this example, whatever it is
# called (dwin-sdk after a plain git clone).
get_filename_component(dwin_sdk_dir "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(dwin_component "${dwin_sdk_dir}" NAME)

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES ${dwin_component} esp_wifi nvs_flash esp_netif esp_event)
