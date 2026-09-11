/***********************************************************************************************************************
 *
 * @file    dwin_ota_downloader.h
 * @brief   DWIN over-the-air update over the Zephyr / nRF Connect downloader.
 * @date    01/10/2025
 * @author  Fernando Fontes
 * @note    Platform Target: Zephyr RTOS with the nRF Connect SDK downloader library
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_OTA_DOWNLOADER_H_
#define DWIN_OTA_DOWNLOADER_H_

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdint.h>
#include <zephyr/types.h>

#include "dwin.h"
#include "dwin_ota.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************** Defines *******************************************************/

/****************************************************** Typedefs ******************************************************/

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/
/**
 *
 * @brief   Initialize the downloader based DWIN OTA client.
 * @param   Obj               Display to update.
 * @param   client_callback   Callback invoked for every OTA event.
 * @return  Returns 0 on success, otherwise a negative error code.
 *
 **/
int dwin_ota_downloader_init(DWIN_Object_t *Obj, dwin_ota_callback_t client_callback);

/**
 *
 * @brief   Start downloading and flashing a comma separated list of DGUS files.
 * @param   hostname       Host the files are served from.
 * @param   files          Comma separated list of files, e.g. "0.icl,23.icl".
 * @param   sec_tag        TLS security tag, or a negative value for plain HTTP.
 * @param   pdn_id         PDN the transfer runs on, 0 for the default one.
 * @param   fragment_size  HTTP range override, in bytes.
 * @note    The files string is rewritten in place by strtok_r and must stay
 *          valid and writable until the transfer ends.
 * @return  Returns 0 on success, otherwise a negative error code.
 *
 **/
int dwin_ota_downloader_start(const char *hostname, const char *files, int sec_tag,
                              uint8_t pdn_id, size_t fragment_size);

/**
 *
 * @brief   Cancel the running transfer.
 * @return  Returns 0 on success, otherwise a negative error code.
 *
 **/
int dwin_ota_downloader_cancel(void);

#ifdef __cplusplus
}
#endif

#endif
