/***********************************************************************************************************************
 *
 * @file    dwin_ota.c
 * @brief   DWIN over-the-air display asset update implementation.
 * @date    01/10/2025
 * @author  Fernando Fontes
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
 *
 * @details The DGUS update path this file drives is:
 *
 *              1. write up to 240 bytes at a time into the variable storage
 *                 update space, starting at word address 0x8000,
 *              2. after 136 full fragments plus one 128 byte fragment the 32KB
 *                 update space is full (136 * 240 + 128 == 32768),
 *              3. ask the display to copy the whole 32KB update space into one
 *                 NOR flash block (VP 0x00AA), then wait 6 seconds,
 *              4. repeat until the file is written. A DGUS file slot is 256KB,
 *                 i.e. 8 consecutive 32KB blocks, so file N starts at block N * 8.
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdint.h>
#include <string.h>

#include "dwin.h"
#include "dwin_ota.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#define DWIN_OTA_MIN(a, b) (((a) < (b)) ? (a) : (b))

/****************************************************** Typedefs ******************************************************/

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_REGISTER();

/***************************************************** Variables ******************************************************/

/*********************************************** Internal Declarations ************************************************/
static uint16_t fragment_capacity_get(const dwin_ota_t *ota);
static int flush_fragment(dwin_ota_t *ota);
static int flush_page(dwin_ota_t *ota);
static void send_progress(dwin_ota_t *ota);

/***************************************************** Functions ******************************************************/
int dwin_ota_begin(dwin_ota_t *ota, const dwin_ota_cfg_t *cfg)
{
    if (ota == NULL || cfg == NULL || cfg->Obj == NULL)
    {
        return -1;
    }

    uint32_t nor_flash_size = (cfg->nor_flash_size != 0) ? cfg->nor_flash_size : DWIN_OTA_NOR_FLASH_SIZE_DEFAULT;
    uint32_t flash_slots = nor_flash_size / DWIN_NOR_FLASH_PAGE_SIZE;

    if (cfg->file_id >= flash_slots)
    {
        DWIN_LOG_ERR("File id %d is out of range, a %u byte NOR flash holds %u file slots",
                     cfg->file_id, (unsigned int)nor_flash_size, (unsigned int)flash_slots);
        return -1;
    }

    /* Number of 32KB flash write operations required by this file. A file larger than
     * one 256KB slot is allowed and simply spills into the following slots. */
    uint32_t number_of_pages = cfg->file_size / DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE;
    if ((cfg->file_size % DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE) != 0)
    {
        number_of_pages++;
    }

    uint32_t flash_pages = flash_slots * DWIN_OTA_PAGES_PER_FILE;
    uint32_t page_base = cfg->file_id * DWIN_OTA_PAGES_PER_FILE;
    uint32_t available_pages = flash_pages - page_base;

    if (number_of_pages > available_pages)
    {
        DWIN_LOG_ERR("File of %u bytes does not fit in NOR flash: needs %u blocks from block %u, %u available",
                     (unsigned int)cfg->file_size, (unsigned int)number_of_pages, (unsigned int)page_base,
                     (unsigned int)available_pages);
        return -1;
    }

    memset(ota, 0, sizeof(*ota));

    ota->Obj = cfg->Obj;
    ota->callback = cfg->callback;
    ota->file_size = cfg->file_size;
    ota->page_base = (uint16_t)page_base;
    ota->flash_pages = (uint16_t)flash_pages;
    ota->number_of_pages = (uint16_t)number_of_pages;
    ota->last_progress = -1;
    ota->active = true;

    DWIN_LOG_INF("OTA started: file id %d, base block %d, %d pages, file size %u",
                 cfg->file_id, ota->page_base, ota->number_of_pages, (unsigned int)ota->file_size);

    if (ota->callback != NULL)
    {
        const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_STARTED};
        ota->callback(&evt);
    }

    return 0;
}

int dwin_ota_write(dwin_ota_t *ota, const uint8_t *pData, size_t len)
{
    if (ota == NULL || !ota->active || (pData == NULL && len > 0))
    {
        return -1;
    }

    while (len > 0)
    {
        uint16_t capacity = fragment_capacity_get(ota);
        uint16_t space = capacity - ota->bytes_in_buf;
        uint16_t chunk = (uint16_t)DWIN_OTA_MIN((size_t)space, len);

        memcpy(&ota->buf[ota->bytes_in_buf], pData, chunk);
        ota->bytes_in_buf += chunk;
        ota->written_size += chunk;
        pData += chunk;
        len -= chunk;

        if (ota->bytes_in_buf == capacity)
        {
            if (flush_fragment(ota) < 0)
            {
                return -1;
            }
        }
    }

    send_progress(ota);

    return 0;
}

int dwin_ota_finish(dwin_ota_t *ota)
{
    if (ota == NULL || !ota->active)
    {
        return -1;
    }

    /* The display always commits a full 32KB block. Fill what the file leaves of the last
     * one with 0xFF, like erased flash, so the previous content of the update space does not
     * end up in the NOR flash behind the file. The last fragment commits the block. */
    while ((ota->bytes_in_buf > 0) || (ota->fragment_index != 0))
    {
        uint16_t capacity = fragment_capacity_get(ota);

        memset(&ota->buf[ota->bytes_in_buf], 0xFF, capacity - ota->bytes_in_buf);
        ota->bytes_in_buf = capacity;

        if (flush_fragment(ota) < 0)
        {
            return -1;
        }
    }

    if ((ota->file_size != 0) && (ota->current_page != ota->number_of_pages))
    {
        DWIN_LOG_ERR("Invalid number of pages: Current page %d, Number of pages %d",
                     ota->current_page, ota->number_of_pages);
        ota->active = false;
        return -1;
    }

    if ((ota->file_size != 0) && (ota->written_size != ota->file_size))
    {
        DWIN_LOG_ERR("Written size does not match file size: %u != %u",
                     (unsigned int)ota->written_size, (unsigned int)ota->file_size);
        ota->active = false;
        return -1;
    }

    DWIN_LOG_INF("OTA finished: %u bytes written in %d blocks",
                 (unsigned int)ota->written_size, ota->current_page);

    ota->active = false;

    if (ota->callback != NULL)
    {
        const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_FILE_FINISHED};
        ota->callback(&evt);
    }

    return 0;
}

int dwin_ota_abort(dwin_ota_t *ota)
{
    if (ota == NULL)
    {
        return -1;
    }

    DWIN_LOG_WRN("OTA aborted after %u bytes", (unsigned int)ota->written_size);

    ota->active = false;
    ota->bytes_in_buf = 0;
    ota->fragment_index = 0;

    return 0;
}

int dwin_ota_progress_get(const dwin_ota_t *ota)
{
    if (ota == NULL || ota->file_size == 0)
    {
        return 0;
    }

    return (int)((ota->written_size * 100U) / ota->file_size);
}

uint32_t dwin_ota_written_size_get(const dwin_ota_t *ota)
{
    return (ota == NULL) ? 0 : ota->written_size;
}

uint16_t dwin_ota_file_id_from_name(const char *file_name)
{
    uint16_t res = 0;

    if (file_name == NULL)
    {
        return 0;
    }

    for (const char *p = file_name; *p != '\0'; p++)
    {
        if (*p >= '0' && *p <= '9')
        {
            res = res * 10 + (*p - '0');
        }
    }

    return res;
}

/************************************************* Internal Functions *************************************************/
/**
 * @brief   Number of bytes the current fragment can carry.
 * @param   In:   ota   Pointer to the OTA context.
 * @note    The 32KB update space holds 136 fragments of 240 bytes plus a final
 *          fragment of 128 bytes.
 * @retval  Capacity of the current fragment, in bytes.
 */
static uint16_t fragment_capacity_get(const dwin_ota_t *ota)
{
    return (ota->fragment_index == DWIN_OTA_FRAGMENTS_PER_PAGE) ? DWIN_OTA_LAST_FRAGMENT_SIZE
                                                                : DWIN_OTA_FRAGMENT_SIZE;
}

/**
 * @brief   Hand the staging buffer over to the display variable storage and, when
 *          the 32KB update space is full, commit it to the NOR flash.
 * @param   In:   ota   Pointer to the OTA context.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int flush_fragment(dwin_ota_t *ota)
{
    DWIN_LOG_DBG("Writing fragment %d of %d bytes to variable storage", ota->fragment_index, ota->bytes_in_buf);

    if (dwin_write_data_to_variable_storage(ota->Obj, ota->buf, ota->bytes_in_buf, ota->fragment_index) < 0)
    {
        DWIN_LOG_ERR("Failed to write fragment %d to variable storage", ota->fragment_index);
        ota->active = false;
        return -1;
    }

    bool page_is_full = (ota->fragment_index == DWIN_OTA_FRAGMENTS_PER_PAGE);

    ota->bytes_in_buf = 0;
    ota->fragment_index++;

    if (page_is_full)
    {
        return flush_page(ota);
    }

    return 0;
}

/**
 * @brief   Copy the 32KB variable storage update space into the next NOR flash block.
 * @param   In:   ota   Pointer to the OTA context.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int flush_page(dwin_ota_t *ota)
{
    uint16_t destination_id = ota->page_base + ota->current_page;

    if ((ota->number_of_pages != 0) && (ota->current_page >= ota->number_of_pages))
    {
        DWIN_LOG_ERR("Invalid number of pages: Current page %d, Number of pages %d",
                     ota->current_page, ota->number_of_pages);
        ota->active = false;
        return -1;
    }

    /* Only reachable with an unknown file size, where begin could not check the fit. */
    if (destination_id >= ota->flash_pages)
    {
        DWIN_LOG_ERR("Block %d is past the end of the NOR flash (%d blocks)", destination_id, ota->flash_pages);
        ota->active = false;
        return -1;
    }

    DWIN_LOG_DBG("Writing block %02X to NOR flash", destination_id);

    if (dwin_write_from_variable_storage_to_nor_flash(ota->Obj, destination_id) < 0)
    {
        DWIN_LOG_ERR("Failed to write block %02X to NOR flash", destination_id);
        ota->active = false;
        return -1;
    }

    ota->fragment_index = 0;
    ota->current_page++;

    return 0;
}

/**
 * @brief   Report the transfer progress, at most once per percent.
 * @param   In:   ota   Pointer to the OTA context.
 */
static void send_progress(dwin_ota_t *ota)
{
    if (ota->callback == NULL || ota->file_size == 0)
    {
        return;
    }

    int progress = dwin_ota_progress_get(ota);
    if (progress == ota->last_progress)
    {
        return;
    }

    ota->last_progress = progress;

    const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_PROGRESS, .progress = progress};
    ota->callback(&evt);
}
