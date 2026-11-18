/***********************************************************************************************************************
 *
 * @file    dwin_ota.h
 * @brief   DWIN over-the-air display asset update header file.
 * @date    01/10/2025
 * @author  Fernando Fontes
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
 *
 * @details Transport agnostic streaming writer for DGUS NOR flash assets (.icl, .bin,
 *          fonts, ...). Feed it the bytes of one DGUS file, from wherever they come
 *          (HTTP, MQTT, SD card, USB, XMODEM), and it takes care of:
 *              - splitting the stream into 240 byte variable storage fragments,
 *              - committing every 32KB block to the NOR flash,
 *              - respecting the mandatory delay between flash write operations.
 *
 * SPDX-License-Identifier: MIT
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_OTA_H_
#define DWIN_OTA_H_

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "dwin.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************** Defines *******************************************************/
/* Bytes carried by one variable storage write: 120 words == 240 bytes. */
#define DWIN_OTA_FRAGMENT_SIZE (DWIN_VARIABLE_STORAGE_FRAGMENT_SIZE * DWIN_VARIABLE_STORAGE_WORD_SIZE)

/* Number of full fragments that fit in one 32KB flash write operation: 136. */
#define DWIN_OTA_FRAGMENTS_PER_PAGE (DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE / DWIN_OTA_FRAGMENT_SIZE)

/* Remainder written by the last fragment of a 32KB block: 128 bytes.
 * 136 * 240 + 128 == 32768. */
#define DWIN_OTA_LAST_FRAGMENT_SIZE (DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE - \
                                     (DWIN_OTA_FRAGMENTS_PER_PAGE * DWIN_OTA_FRAGMENT_SIZE))

/* Number of 32KB flash write operations needed to fill one 256KB file slot: 8. */
#define DWIN_OTA_PAGES_PER_FILE (DWIN_NOR_FLASH_PAGE_SIZE / DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE)

/* NOR flash sizes found on DGUS displays. Most units carry 16MB, some low cost ones 8MB. */
#define DWIN_OTA_NOR_FLASH_SIZE_8MB  (8UL * 1024UL * 1024UL)
#define DWIN_OTA_NOR_FLASH_SIZE_16MB (16UL * 1024UL * 1024UL)

/* NOR flash size assumed when dwin_ota_cfg_t.nor_flash_size is left at 0. */
#define DWIN_OTA_NOR_FLASH_SIZE_DEFAULT DWIN_OTA_NOR_FLASH_SIZE_16MB

/****************************************************** Typedefs ******************************************************/
/**
 * @brief DWIN OTA event IDs.
 *
 * After FINISHED, ERROR or CANCELLED event, the DWIN OTA client is ready to
 * accept a new request.
 */
enum dwin_ota_evt_id
{
    /** A new transfer has been accepted and started. */
    DWIN_OTA_EVT_STARTED,

    /** Transfer progress report, in percent. */
    DWIN_OTA_EVT_PROGRESS,

    /** One file of the transfer has been fully committed to the NOR flash. */
    DWIN_OTA_EVT_FILE_FINISHED,

    /** The whole transfer was completed successfully. */
    DWIN_OTA_EVT_FINISHED,

    /** DWIN OTA abandoned due to an error. */
    DWIN_OTA_EVT_ERROR,

    /** DWIN OTA abandoned due to a cancellation request. */
    DWIN_OTA_EVT_CANCELLED,
};

/**
 * @brief DWIN OTA error cause values.
 */
enum dwin_ota_error_cause
{
    /** No error, used when event ID is not DWIN_OTA_EVT_ERROR. */
    DWIN_OTA_ERROR_CAUSE_NO_ERROR,
    /** Connecting to the update server failed. A possible reason could be wrong
     *  TLS credentials. A retry is unlikely to help with the same credentials.
     */
    DWIN_OTA_ERROR_CAUSE_CONNECT_FAILED,
    /** Downloading the update failed. The download may be retried. */
    DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED,
    /** The update is invalid and was rejected. Retry will not help. */
    DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE,
    /** The display rejected a write or did not acknowledge it. */
    DWIN_OTA_ERROR_CAUSE_DISPLAY,
    /** Generic error on device side. */
    DWIN_OTA_ERROR_CAUSE_INTERNAL,
    /** Protocol not supported. */
    DWIN_OTA_ERROR_CAUSE_PROTO_NOT_SUPPORTED,
    /** Invalid URI or invalid configuration. */
    DWIN_OTA_ERROR_CAUSE_INVALID_CONFIGURATION,
};

struct dwin_ota_evt
{
    enum dwin_ota_evt_id id;

    union
    {
        /** Error cause. */
        enum dwin_ota_error_cause cause;
        /** Transfer progress %. */
        int progress;
    };
};

typedef void (*dwin_ota_callback_t)(const struct dwin_ota_evt *evt);

/**
 * @brief Configuration of a single file transfer.
 */
typedef struct
{
    /** Display this file is written to. */
    DWIN_Object_t *Obj;
    /** DGUS file index, i.e. the number in front of the file name (23.icl -> 23). */
    uint16_t file_id;
    /** Total size of the file, in bytes. Set to 0 when unknown. */
    uint32_t file_size;
    /** Display NOR flash size, in bytes (DWIN_OTA_NOR_FLASH_SIZE_*).
     *  Set to 0 to use DWIN_OTA_NOR_FLASH_SIZE_DEFAULT. */
    uint32_t nor_flash_size;
    /** Optional progress callback, may be NULL. */
    dwin_ota_callback_t callback;
} dwin_ota_cfg_t;

/**
 * @brief State of a single file transfer. Treat as opaque.
 */
typedef struct
{
    DWIN_Object_t *Obj;
    dwin_ota_callback_t callback;
    uint16_t page_base;
    uint16_t flash_pages;
    uint32_t file_size;
    uint32_t written_size;
    uint16_t number_of_pages;
    uint16_t current_page;
    uint16_t fragment_index;
    uint16_t bytes_in_buf;
    int last_progress;
    bool active;
    uint8_t buf[DWIN_OTA_FRAGMENT_SIZE];
} dwin_ota_t;

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/
/**
 *
 * @brief   Start a new file transfer.
 * @param   ota   Pointer to the OTA context to initialize.
 * @param   cfg   Transfer configuration.
 * @note    The caller is responsible for pausing the display (dwin_pause_display)
 *          before the first file and resetting it (dwin_reset_display) after the
 *          last one.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_begin(dwin_ota_t *ota, const dwin_ota_cfg_t *cfg);

/**
 *
 * @brief   Feed the next chunk of the file to the display.
 * @param   ota   Pointer to the OTA context.
 * @param   pData Pointer to the chunk.
 * @param   len   Length of the chunk, in bytes. Any length is accepted.
 * @note    This call blocks while the display commits a 32KB block to the NOR
 *          flash, which takes DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_write(dwin_ota_t *ota, const uint8_t *pData, size_t len);

/**
 *
 * @brief   Flush the pending bytes and commit the last 32KB block.
 * @param   ota   Pointer to the OTA context.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_finish(dwin_ota_t *ota);

/**
 *
 * @brief   Abandon the current transfer and release the context.
 * @param   ota   Pointer to the OTA context.
 * @note    The already committed 32KB blocks are not rolled back.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_abort(dwin_ota_t *ota);

/**
 *
 * @brief   Get the transfer progress, in percent.
 * @param   ota   Pointer to the OTA context.
 * @return  Returns the progress in the range 0..100, or 0 when the file size is unknown.
 *
 **/
int dwin_ota_progress_get(const dwin_ota_t *ota);

/**
 *
 * @brief   Get the number of bytes handed over to the display so far.
 * @param   ota   Pointer to the OTA context.
 * @return  Returns the number of bytes written.
 *
 **/
uint32_t dwin_ota_written_size_get(const dwin_ota_t *ota);

/**
 *
 * @brief   Derive the DGUS file index from a file name.
 * @param   file_name  File name or path, e.g. "23.icl" or "/assets/23.icl".
 * @note    Every decimal digit found in the string is concatenated, so a path
 *          prefix is tolerated as long as it carries no digits of its own.
 * @return  Returns the file index.
 *
 **/
uint16_t dwin_ota_file_id_from_name(const char *file_name);

#ifdef __cplusplus
}
#endif

#endif
