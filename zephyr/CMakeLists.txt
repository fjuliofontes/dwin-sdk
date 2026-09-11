# DWIN SDK - Zephyr module.
#
# Add the SDK to your west manifest, or point ZEPHYR_EXTRA_MODULES at the
# checkout, then enable CONFIG_DWIN=y.

if(CONFIG_DWIN)
    zephyr_library()
    zephyr_library_sources(
        ${CMAKE_CURRENT_LIST_DIR}/../src/dwin.c
        ${CMAKE_CURRENT_LIST_DIR}/../src/dwin_ota.c)

    if(CONFIG_DWIN_PORT)
        zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../ports/zephyr/dwin_port_zephyr.c)
    endif()

    if(CONFIG_DWIN_OTA_DOWNLOADER)
        zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../ports/zephyr/dwin_ota_downloader_zephyr.c)
    endif()

    zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)
endif()
