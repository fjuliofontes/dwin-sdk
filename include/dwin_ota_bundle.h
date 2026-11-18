/***********************************************************************************************************************
 *
 * @file    dwin_ota_bundle.h
 * @brief   DWIN over-the-air display update from a single .dwpk bundle, header file.
 * @date    17/11/2026
 * @author  Fernando Fontes
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
 *
 * @details A .dwpk bundle carries every DGUS file of a display update in one download. It is
 *          built by tools/dwin_pack.py and starts with a table that tells where each file sits
 *          in the bundle and which DGUS file slot it goes to. Every field is little-endian.
 *
 *              Header, 32 bytes
 *                  0   4   magic "DWPK"
 *                  4   1   format version (1)
 *                  5   1   flags (0)
 *                  6   2   number of files
 *                  8   4   total bundle size
 *                  12  16  version label, ASCII, zero padded
 *                  28  4   CRC32 of the header (without this field) followed by every entry
 *
 *              One entry per file, 16 bytes
 *                  0   2   DGUS file id (23.icl -> 23)
 *                  2   2   reserved (0)
 *                  4   4   offset of the file data in the bundle
 *                  8   4   size of the file data
 *                  12  4   CRC32 of the file data
 *
 *              File data, in the order of the entries.
 *
 *          Feed the bundle bytes in order, from wherever they come. The table is checked as a
 *          whole before the display is touched, then every file is streamed into the NOR flash
 *          through dwin_ota.
 *
 * SPDX-License-Identifier: MIT
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_OTA_BUNDLE_H_
#define DWIN_OTA_BUNDLE_H_

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "dwin.h"
#include "dwin_ota.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************** Defines *******************************************************/
#if defined(CONFIG_DWIN_OTA_BUNDLE_MAX_FILES) && !defined(DWIN_OTA_BUNDLE_MAX_FILES)
#define DWIN_OTA_BUNDLE_MAX_FILES CONFIG_DWIN_OTA_BUNDLE_MAX_FILES
#endif

/* Largest number of files one bundle may carry. The file table is kept in RAM for the whole
 * transfer, 16 bytes per file. */
#ifndef DWIN_OTA_BUNDLE_MAX_FILES
#define DWIN_OTA_BUNDLE_MAX_FILES 16
#endif

#define DWIN_OTA_BUNDLE_HEADER_SIZE 32
#define DWIN_OTA_BUNDLE_ENTRY_SIZE 16

/* Version label of a bundle, including the null terminator. */
#define DWIN_OTA_BUNDLE_VERSION_SIZE 16

/****************************************************** Typedefs ******************************************************/
/**
 * @brief Configuration of a bundle transfer.
 */
typedef struct
{
    /** Display the bundle is written to. */
    DWIN_Object_t *Obj;
    /** Display NOR flash size, in bytes (DWIN_OTA_NOR_FLASH_SIZE_*).
     *  Set to 0 to use DWIN_OTA_NOR_FLASH_SIZE_DEFAULT. */
    uint32_t nor_flash_size;
    /** Oldest version accepted, dotted numeric ("1.4.0"). A bundle whose version label is lower
     *  is rejected before the display is touched. NULL accepts any version. The string
     *  must stay valid until the transfer ends. */
    const char *min_version;
    /** Optional callback, may be NULL. Receives DWIN_OTA_EVT_PROGRESS, over the whole bundle,
     *  and DWIN_OTA_EVT_FILE_FINISHED. */
    dwin_ota_callback_t callback;
} dwin_ota_bundle_cfg_t;

/**
 * @brief One file of the bundle.
 */
typedef struct
{
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint16_t file_id;
} dwin_ota_bundle_entry_t;

/**
 * @brief State of a bundle transfer. Treat as opaque.
 */
typedef struct
{
    dwin_ota_bundle_cfg_t cfg;
    dwin_ota_t ota;
    dwin_ota_bundle_entry_t entries[DWIN_OTA_BUNDLE_MAX_FILES];
    char version[DWIN_OTA_BUNDLE_VERSION_SIZE];
    uint8_t staging[DWIN_OTA_BUNDLE_HEADER_SIZE];
    uint8_t staged;
    uint8_t state;
    uint8_t cause;
    bool display_paused;
    bool file_open;
    uint16_t file_count;
    uint16_t file_index;
    uint32_t stream_pos;
    uint32_t total_size;
    uint32_t table_crc;
    uint32_t crc;
    uint32_t file_remaining;
    uint32_t data_size;
    uint32_t data_written;
    int last_progress;
} dwin_ota_bundle_t;

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/
/**
 *
 * @brief   Start a new bundle transfer.
 * @param   bundle  Pointer to the bundle context to initialize.
 * @param   cfg     Transfer configuration.
 * @note    Nothing is sent to the display yet. It is paused once the file table has been
 *          received and accepted.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_bundle_begin(dwin_ota_bundle_t *bundle, const dwin_ota_bundle_cfg_t *cfg);

/**
 *
 * @brief   Feed the next chunk of the bundle.
 * @param   bundle  Pointer to the bundle context.
 * @param   pData   Pointer to the chunk.
 * @param   len     Length of the chunk, in bytes. Any length is accepted.
 * @note    This call blocks while the display commits a 32KB block to the NOR flash.
 * @note    A file whose CRC does not match is only detected once it is in the NOR flash. The
 *          transfer stops there, so the files that follow it are not written.
 * @return  Returns 0 on success, otherwise an error code. The reason is given by
 *          dwin_ota_bundle_error_cause_get() and the transfer cannot be continued.
 *
 **/
int dwin_ota_bundle_write(dwin_ota_bundle_t *bundle, const uint8_t *pData, size_t len);

/**
 *
 * @brief   Close the transfer and reset the display so it boots into the new files.
 * @param   bundle  Pointer to the bundle context.
 * @note    Fails with DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED when the bundle is not complete yet.
 *          The context is left untouched in that case, so the transfer can still be resumed
 *          with dwin_ota_bundle_rewind().
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_bundle_finish(dwin_ota_bundle_t *bundle);

/**
 *
 * @brief   Abandon the transfer.
 * @param   bundle  Pointer to the bundle context.
 * @note    The display is reset when it had already been paused. The blocks that were committed
 *          to the NOR flash are not rolled back.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_bundle_abort(dwin_ota_bundle_t *bundle);

/**
 *
 * @brief   Step back to the start of the file being received, to resume a broken download.
 * @param   bundle  Pointer to the bundle context.
 * @param   offset  Bundle offset the next dwin_ota_bundle_write() must start from, typically
 *                  requested from the server with an HTTP Range header.
 * @note    The files already committed are kept. The offset is 0 while the file table is still
 *          being received.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_bundle_rewind(dwin_ota_bundle_t *bundle, uint32_t *offset);

/**
 *
 * @brief   Step back to the first byte of the bundle, for a source that cannot resume.
 * @param   bundle  Pointer to the bundle context.
 * @note    Every file is written again, the display stays paused.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_ota_bundle_restart(dwin_ota_bundle_t *bundle);

/**
 *
 * @brief   Check whether every file of the bundle has been committed to the NOR flash.
 * @param   bundle  Pointer to the bundle context.
 * @return  Returns true once the bundle is complete.
 *
 **/
bool dwin_ota_bundle_is_complete(const dwin_ota_bundle_t *bundle);

/**
 *
 * @brief   Get the transfer progress over the whole bundle, in percent.
 * @param   bundle  Pointer to the bundle context.
 * @return  Returns the progress in the range 0..100, or 0 while the file table is being received.
 *
 **/
int dwin_ota_bundle_progress_get(const dwin_ota_bundle_t *bundle);

/**
 *
 * @brief   Get the version label of the bundle.
 * @param   bundle  Pointer to the bundle context.
 * @return  Returns the version label, an empty string while the header has not been received.
 *
 **/
const char *dwin_ota_bundle_version_get(const dwin_ota_bundle_t *bundle);

/**
 *
 * @brief   Get the reason of the last failure.
 * @param   bundle  Pointer to the bundle context.
 * @return  Returns the error cause, DWIN_OTA_ERROR_CAUSE_NO_ERROR when nothing failed.
 *
 **/
enum dwin_ota_error_cause dwin_ota_bundle_error_cause_get(const dwin_ota_bundle_t *bundle);

#ifdef __cplusplus
}
#endif

#endif
