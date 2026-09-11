/***********************************************************************************************************************
 *
 * @file    dwin_ota_http.h
 * @brief   DWIN over-the-air update over HTTP(S) for ESP-IDF.
 * @date    11/09/2026
 * @author  Fernando Fontes
 * @note    Platform Target: ESP32 family (ESP-IDF v5.x)
 *
 * @details Convenience layer on top of dwin_ota: downloads a comma separated list
 *          of DGUS files from an HTTP(S) server and streams them straight into the
 *          display NOR flash. The whole transfer runs in its own task and reports
 *          through the dwin_ota event callback.
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_OTA_HTTP_H_
#define DWIN_OTA_HTTP_H_

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdbool.h>
#include <stdint.h>

#include "dwin.h"
#include "dwin_ota.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************** Defines *******************************************************/

/****************************************************** Typedefs ******************************************************/
typedef struct
{
    /** Display to update. */
    DWIN_Object_t *Obj;
    /** Base URL the files hang from, without the trailing slash,
     *  e.g. "https://assets.example.com/display/v3". */
    const char *base_url;
    /** Comma separated list of DGUS files, e.g. "0.icl,23.icl,32.icl". */
    const char *files;
    /** PEM encoded server certificate for HTTPS. NULL uses the global CA store. */
    const char *cert_pem;
    /** Skip the server certificate check. Use for development only. */
    bool skip_cert_common_name_check;
    /** HTTP transaction timeout, in milliseconds. 0 selects 10000. */
    int timeout_ms;
    /** Number of socket level retries per file. 0 selects 2. */
    int retries;
    /** Priority of the OTA task. 0 selects 5. */
    int task_priority;
    /** Stack size of the OTA task, in bytes. 0 selects 8192. */
    int task_stack_size;
} dwin_ota_http_cfg_t;

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/
/**
 *
 * @brief   Initialize the HTTP OTA client.
 * @param   client_callback  Callback invoked for every OTA event. Runs in the OTA task.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_http_init(dwin_ota_callback_t client_callback);

/**
 *
 * @brief   Start downloading and flashing the configured list of files.
 * @param   cfg   Transfer configuration. The strings it points to must stay valid
 *                until the transfer ends.
 * @note    The display is paused before the first file and reset after the last one.
 * @return  Returns 0 when the transfer was accepted, otherwise an error code.
 *
 **/
int dwin_ota_http_start(const dwin_ota_http_cfg_t *cfg);

/**
 *
 * @brief   Request the cancellation of the running transfer.
 * @note    Returns as soon as the request is registered, the CANCELLED event is
 *          emitted once the OTA task has actually stopped.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_http_cancel(void);

/**
 *
 * @brief   Check whether a transfer is in progress.
 * @return  Returns true while a transfer is running.
 *
 **/
bool dwin_ota_http_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
