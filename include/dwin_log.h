/***********************************************************************************************************************
 *
 * @file    dwin_log.h
 * @brief   DWIN logging abstraction header.
 * @date    07/11/2025
 * @author  Fernando Fontes
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_LOG_H_
#define DWIN_LOG_H_

/****************************************************** Includes ******************************************************/

/****************************************************** Defines *******************************************************/
/* Allow the application to drop in its own logging backend by defining
 * DWIN_LOG_CUSTOM and providing every DWIN_LOG_* macro before including. */
#if defined(DWIN_LOG_CUSTOM)
/* Nothing to do, the application provides the macros. */

/*------------------------------------------------- ESP-IDF backend --------------------------------------------------*/
#elif defined(ESP_PLATFORM)

#include "esp_log.h"

#define DWIN_LOG_MODULE_REGISTER() static const char *DWIN_LOG_TAG = "dwin"
#define DWIN_LOG_MODULE_DECLARE()  extern const char *DWIN_LOG_TAG

#define DWIN_LOG_ERR(...) ESP_LOGE(DWIN_LOG_TAG, __VA_ARGS__)
#define DWIN_LOG_WRN(...) ESP_LOGW(DWIN_LOG_TAG, __VA_ARGS__)
#define DWIN_LOG_INF(...) ESP_LOGI(DWIN_LOG_TAG, __VA_ARGS__)
#define DWIN_LOG_DBG(...) ESP_LOGD(DWIN_LOG_TAG, __VA_ARGS__)
#define DWIN_LOG_HEXDUMP_DBG(data, len, str) ESP_LOG_BUFFER_HEXDUMP(DWIN_LOG_TAG, data, len, ESP_LOG_DEBUG)
#define DWIN_LOG_HEXDUMP_INF(data, len, str) ESP_LOG_BUFFER_HEXDUMP(DWIN_LOG_TAG, data, len, ESP_LOG_INFO)

/*-------------------------------------------------- Zephyr backend --------------------------------------------------*/
#elif defined(__ZEPHYR__)

#include <zephyr/logging/log.h>

#ifndef CONFIG_DWIN_LOG_LEVEL
#define CONFIG_DWIN_LOG_LEVEL LOG_LEVEL_INF
#endif

#define DWIN_LOG_MODULE_REGISTER() LOG_MODULE_REGISTER(dwin, CONFIG_DWIN_LOG_LEVEL)
#define DWIN_LOG_MODULE_DECLARE()  LOG_MODULE_DECLARE(dwin, CONFIG_DWIN_LOG_LEVEL)

#define DWIN_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#define DWIN_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#define DWIN_LOG_INF(...) LOG_INF(__VA_ARGS__)
#define DWIN_LOG_DBG(...) LOG_DBG(__VA_ARGS__)
#define DWIN_LOG_HEXDUMP_DBG(data, len, str) LOG_HEXDUMP_DBG(data, len, str)
#define DWIN_LOG_HEXDUMP_INF(data, len, str) LOG_HEXDUMP_INF(data, len, str)

/*------------------------------------------ Bare-metal / printf fallback --------------------------------------------*/
#else

#include <stdio.h>

/* Expand to an incomplete type declaration so the trailing semicolon of
 * DWIN_LOG_MODULE_REGISTER(); is not a stray one at file scope. */
#define DWIN_LOG_MODULE_REGISTER() struct dwin_log_module_unused
#define DWIN_LOG_MODULE_DECLARE()  struct dwin_log_module_unused

#ifndef DWIN_LOG_PRINTF
#define DWIN_LOG_PRINTF(...) printf(__VA_ARGS__)
#endif

/* Split into three calls so no compiler extension is needed to swallow the
 * comma of an empty __VA_ARGS__. */
#define DWIN_LOG_AT(level, ...)            \
    do                                     \
    {                                      \
        DWIN_LOG_PRINTF("[dwin][" level "] "); \
        DWIN_LOG_PRINTF(__VA_ARGS__);      \
        DWIN_LOG_PRINTF("\r\n");           \
    } while (0)

#define DWIN_LOG_ERR(...) DWIN_LOG_AT("E", __VA_ARGS__)
#define DWIN_LOG_WRN(...) DWIN_LOG_AT("W", __VA_ARGS__)
#define DWIN_LOG_INF(...) DWIN_LOG_AT("I", __VA_ARGS__)
#define DWIN_LOG_DBG(...) DWIN_LOG_AT("D", __VA_ARGS__)
#define DWIN_LOG_HEXDUMP_DBG(data, len, str) (void)(data)
#define DWIN_LOG_HEXDUMP_INF(data, len, str) (void)(data)

#endif

/****************************************************** Typedefs ******************************************************/

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/

#endif
