# DWIN SDK - ESP-IDF component registration.
#
# Drop this folder into <project>/components/, or add it with the component
# manager, and add `dwin` to your own component REQUIRES.

set(dwin_srcs
    "src/dwin.c"
    "src/dwin_ota.c"
    "ports/esp-idf/dwin_port_esp_idf.c")

set(dwin_requires driver esp_timer)
set(dwin_priv_requires "")

if(CONFIG_DWIN_OTA_HTTP)
    list(APPEND dwin_srcs "ports/esp-idf/dwin_ota_http_esp_idf.c")
    list(APPEND dwin_priv_requires esp_http_client)
endif()

idf_component_register(
    SRCS ${dwin_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${dwin_requires}
    PRIV_REQUIRES ${dwin_priv_requires})
