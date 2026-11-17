/***********************************************************************************************************************
 *
 * @file    dwin_ota_bundle.c
 * @brief   DWIN over-the-air display update from a single .dwpk bundle, implementation.
 * @date    17/11/2026
 * @author  Fernando Fontes
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
 *
 * @details The bundle is consumed as a stream, in three steps:
 *
 *              1. the 32 byte header, which gives the number of files,
 *              2. the file table, one 16 byte entry per file. Once it is complete the whole
 *                 table is checked (CRC, version, room in the NOR flash) and only then is the
 *                 display paused,
 *              3. the data of every file, handed over to dwin_ota one file at a time.
 *
 *          The layout of the bundle is described in dwin_ota_bundle.h.
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
#include "dwin_ota_bundle.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#define DWIN_OTA_BUNDLE_MIN(a, b) (((a) < (b)) ? (a) : (b))

#define DWIN_OTA_BUNDLE_FORMAT_VERSION 1

/* Offsets inside the header. */
#define DWIN_OTA_BUNDLE_HDR_FORMAT 4
#define DWIN_OTA_BUNDLE_HDR_FILE_COUNT 6
#define DWIN_OTA_BUNDLE_HDR_TOTAL_SIZE 8
#define DWIN_OTA_BUNDLE_HDR_VERSION 12
#define DWIN_OTA_BUNDLE_HDR_CRC 28

/* Offsets inside a file table entry. */
#define DWIN_OTA_BUNDLE_ENTRY_FILE_ID 0
#define DWIN_OTA_BUNDLE_ENTRY_OFFSET 4
#define DWIN_OTA_BUNDLE_ENTRY_FILE_SIZE 8
#define DWIN_OTA_BUNDLE_ENTRY_CRC 12

/****************************************************** Typedefs ******************************************************/
enum dwin_ota_bundle_state
{
    /** No transfer running. */
    BUNDLE_STATE_IDLE,
    /** Receiving the header. */
    BUNDLE_STATE_HEADER,
    /** Receiving the file table. */
    BUNDLE_STATE_TABLE,
    /** Receiving the files. */
    BUNDLE_STATE_DATA,
    /** Every file is in the NOR flash. */
    BUNDLE_STATE_DONE,
    /** The transfer hit an error it cannot recover from. */
    BUNDLE_STATE_FAILED,
};

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_REGISTER();

static const uint8_t bundle_magic[] = {'D', 'W', 'P', 'K'};

/***************************************************** Variables ******************************************************/

/*********************************************** Internal Declarations ************************************************/
static uint16_t read_le16(const uint8_t *p);
static uint32_t read_le32(const uint8_t *p);
static uint32_t crc32_update(uint32_t crc, const uint8_t *pData, size_t len);
static bool version_is_valid(const char *version);
static int version_compare(const char *a, const char *b);
static int fail(dwin_ota_bundle_t *bundle, enum dwin_ota_error_cause cause);
static void reset_parser(dwin_ota_bundle_t *bundle);
static void close_file(dwin_ota_bundle_t *bundle);
static int parse_header(dwin_ota_bundle_t *bundle);
static void parse_entry(dwin_ota_bundle_t *bundle);
static int validate_table(dwin_ota_bundle_t *bundle);
static int write_file_data(dwin_ota_bundle_t *bundle, const uint8_t *pData, size_t len, size_t *consumed);
static void send_progress(dwin_ota_bundle_t *bundle);

/***************************************************** Functions ******************************************************/
int dwin_ota_bundle_begin(dwin_ota_bundle_t *bundle, const dwin_ota_bundle_cfg_t *cfg)
{
    if (bundle == NULL || cfg == NULL || cfg->Obj == NULL)
    {
        return -1;
    }

    if ((cfg->min_version != NULL) && !version_is_valid(cfg->min_version))
    {
        DWIN_LOG_ERR("Minimum version \"%s\" is not a dotted numeric version", cfg->min_version);
        return -1;
    }

    memset(bundle, 0, sizeof(*bundle));

    bundle->cfg = *cfg;
    bundle->cause = DWIN_OTA_ERROR_CAUSE_NO_ERROR;

    reset_parser(bundle);

    return 0;
}

int dwin_ota_bundle_write(dwin_ota_bundle_t *bundle, const uint8_t *pData, size_t len)
{
    if (bundle == NULL || (pData == NULL && len > 0))
    {
        return -1;
    }

    while (len > 0)
    {
        size_t consumed = 0;

        switch (bundle->state)
        {
        case BUNDLE_STATE_HEADER:
            consumed = DWIN_OTA_BUNDLE_MIN(len, (size_t)(DWIN_OTA_BUNDLE_HEADER_SIZE - bundle->staged));
            memcpy(&bundle->staging[bundle->staged], pData, consumed);
            bundle->staged += (uint8_t)consumed;

            if (bundle->staged == DWIN_OTA_BUNDLE_HEADER_SIZE)
            {
                if (parse_header(bundle) < 0)
                {
                    return -1;
                }
                bundle->staged = 0;
                bundle->state = BUNDLE_STATE_TABLE;
            }
            break;

        case BUNDLE_STATE_TABLE:
            consumed = DWIN_OTA_BUNDLE_MIN(len, (size_t)(DWIN_OTA_BUNDLE_ENTRY_SIZE - bundle->staged));
            memcpy(&bundle->staging[bundle->staged], pData, consumed);
            bundle->staged += (uint8_t)consumed;

            if (bundle->staged == DWIN_OTA_BUNDLE_ENTRY_SIZE)
            {
                parse_entry(bundle);
                bundle->staged = 0;

                if (bundle->file_index == bundle->file_count)
                {
                    if (validate_table(bundle) < 0)
                    {
                        return -1;
                    }
                    bundle->file_index = 0;
                    bundle->state = BUNDLE_STATE_DATA;
                }
            }
            break;

        case BUNDLE_STATE_DATA:
            if (write_file_data(bundle, pData, len, &consumed) < 0)
            {
                return -1;
            }
            break;

        case BUNDLE_STATE_DONE:
            /* Nothing is expected after the last file, drop it. */
            consumed = len;
            break;

        default:
            return -1;
        }

        bundle->stream_pos += (uint32_t)consumed;
        pData += consumed;
        len -= consumed;
    }

    return 0;
}

int dwin_ota_bundle_finish(dwin_ota_bundle_t *bundle)
{
    if (bundle == NULL || bundle->state == BUNDLE_STATE_IDLE || bundle->state == BUNDLE_STATE_FAILED)
    {
        return -1;
    }

    if (bundle->state != BUNDLE_STATE_DONE)
    {
        DWIN_LOG_ERR("Bundle is not complete, stopped at byte %u", (unsigned int)bundle->stream_pos);
        bundle->cause = DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED;
        return -1;
    }

    /* The display only reads the new files when it boots. */
    if (dwin_reset_display(bundle->cfg.Obj) < 0)
    {
        DWIN_LOG_ERR("Failed to reset the DWIN display");
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_DISPLAY);
    }

    DWIN_LOG_INF("Bundle %s written: %d files, %u bytes", bundle->version, bundle->file_count,
                 (unsigned int)bundle->data_written);

    bundle->display_paused = false;
    bundle->state = BUNDLE_STATE_IDLE;

    return 0;
}

int dwin_ota_bundle_abort(dwin_ota_bundle_t *bundle)
{
    if (bundle == NULL)
    {
        return -1;
    }

    DWIN_LOG_WRN("Bundle transfer aborted at byte %u", (unsigned int)bundle->stream_pos);

    close_file(bundle);

    /* A paused display does not draw anything, bring it back with whatever it holds now. */
    if (bundle->display_paused)
    {
        if (dwin_reset_display(bundle->cfg.Obj) < 0)
        {
            DWIN_LOG_ERR("Failed to reset the DWIN display");
        }
        bundle->display_paused = false;
    }

    bundle->state = BUNDLE_STATE_IDLE;

    return 0;
}

int dwin_ota_bundle_rewind(dwin_ota_bundle_t *bundle, uint32_t *offset)
{
    if (bundle == NULL || offset == NULL)
    {
        return -1;
    }

    switch (bundle->state)
    {
    case BUNDLE_STATE_HEADER:
    case BUNDLE_STATE_TABLE:
        reset_parser(bundle);
        break;

    case BUNDLE_STATE_DATA:
    {
        const dwin_ota_bundle_entry_t *entry = &bundle->entries[bundle->file_index];

        if (bundle->file_open)
        {
            bundle->data_written -= (entry->size - bundle->file_remaining);
            close_file(bundle);
        }

        bundle->stream_pos = entry->offset;
        break;
    }

    default:
        return -1;
    }

    bundle->cause = DWIN_OTA_ERROR_CAUSE_NO_ERROR;
    *offset = bundle->stream_pos;

    DWIN_LOG_INF("Bundle transfer resumes from byte %u", (unsigned int)bundle->stream_pos);

    return 0;
}

int dwin_ota_bundle_restart(dwin_ota_bundle_t *bundle)
{
    if (bundle == NULL || bundle->state == BUNDLE_STATE_IDLE || bundle->state == BUNDLE_STATE_FAILED)
    {
        return -1;
    }

    close_file(bundle);
    reset_parser(bundle);

    bundle->cause = DWIN_OTA_ERROR_CAUSE_NO_ERROR;

    return 0;
}

bool dwin_ota_bundle_is_complete(const dwin_ota_bundle_t *bundle)
{
    return (bundle != NULL) && (bundle->state == BUNDLE_STATE_DONE);
}

int dwin_ota_bundle_progress_get(const dwin_ota_bundle_t *bundle)
{
    if (bundle == NULL || bundle->data_size == 0)
    {
        return 0;
    }

    /* A display NOR flash holds 16MB at most, so this cannot overflow. */
    return (int)((bundle->data_written * 100U) / bundle->data_size);
}

const char *dwin_ota_bundle_version_get(const dwin_ota_bundle_t *bundle)
{
    return (bundle == NULL) ? "" : bundle->version;
}

enum dwin_ota_error_cause dwin_ota_bundle_error_cause_get(const dwin_ota_bundle_t *bundle)
{
    return (bundle == NULL) ? DWIN_OTA_ERROR_CAUSE_INTERNAL : (enum dwin_ota_error_cause)bundle->cause;
}

/************************************************* Internal Functions *************************************************/
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief   Standard CRC32 (the zlib one), computed bit by bit to save the 1KB lookup table.
 * @param   In:   crc     CRC of the data seen so far, 0 to start.
 * @param   In:   pData   Data to add.
 * @param   In:   len     Length of the data, in bytes.
 * @note    The display takes 6 seconds per 32KB block, the CRC is never the slow part.
 * @retval  Updated CRC.
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *pData, size_t len)
{
    crc = ~crc;

    while (len-- > 0)
    {
        crc ^= *pData++;
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }

    return ~crc;
}

/**
 * @brief   Check that a version is dotted numeric, "1.4.0" style.
 * @param   In:   version   Version to check.
 * @retval  Returns true when the version can be compared.
 */
static bool version_is_valid(const char *version)
{
    bool digit_seen = false;

    for (const char *p = version; *p != '\0'; p++)
    {
        if (*p >= '0' && *p <= '9')
        {
            digit_seen = true;
        }
        else if (*p == '.' && digit_seen)
        {
            digit_seen = false;
        }
        else
        {
            return false;
        }
    }

    /* Also rejects an empty version and a trailing dot. */
    return digit_seen;
}

/**
 * @brief   Compare two dotted numeric versions field by field, a missing field counts as 0.
 * @param   In:   a   First version.
 * @param   In:   b   Second version.
 * @retval  Negative when a is older than b, 0 when they match, positive when a is newer.
 */
static int version_compare(const char *a, const char *b)
{
    while (*a != '\0' || *b != '\0')
    {
        uint32_t field_a = 0;
        uint32_t field_b = 0;

        while (*a >= '0' && *a <= '9')
        {
            field_a = field_a * 10 + (uint32_t)(*a++ - '0');
        }
        while (*b >= '0' && *b <= '9')
        {
            field_b = field_b * 10 + (uint32_t)(*b++ - '0');
        }

        if (field_a != field_b)
        {
            return (field_a < field_b) ? -1 : 1;
        }

        if (*a == '.')
        {
            a++;
        }
        if (*b == '.')
        {
            b++;
        }
    }

    return 0;
}

/**
 * @brief   Stop the transfer for good.
 * @param   In:   bundle   Pointer to the bundle context.
 * @param   In:   cause    Reason of the failure.
 * @retval  Always -1, so it can be returned directly.
 */
static int fail(dwin_ota_bundle_t *bundle, enum dwin_ota_error_cause cause)
{
    close_file(bundle);

    bundle->cause = (uint8_t)cause;
    bundle->state = BUNDLE_STATE_FAILED;

    return -1;
}

/**
 * @brief   Go back to waiting for the first byte of the bundle.
 * @param   In:   bundle   Pointer to the bundle context.
 * @note    The display is left as it is, paused or not.
 */
static void reset_parser(dwin_ota_bundle_t *bundle)
{
    bundle->state = BUNDLE_STATE_HEADER;
    bundle->staged = 0;
    bundle->file_count = 0;
    bundle->file_index = 0;
    bundle->stream_pos = 0;
    bundle->data_size = 0;
    bundle->data_written = 0;
    bundle->last_progress = -1;
}

/**
 * @brief   Drop the file being written, if any.
 * @param   In:   bundle   Pointer to the bundle context.
 */
static void close_file(dwin_ota_bundle_t *bundle)
{
    if (bundle->file_open)
    {
        dwin_ota_abort(&bundle->ota);
        bundle->file_open = false;
    }
}

/**
 * @brief   Read the header out of the staging buffer.
 * @param   In:   bundle   Pointer to the bundle context.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int parse_header(dwin_ota_bundle_t *bundle)
{
    const uint8_t *header = bundle->staging;

    if (memcmp(header, bundle_magic, sizeof(bundle_magic)) != 0)
    {
        DWIN_LOG_ERR("Not a .dwpk bundle");
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
    }

    if (header[DWIN_OTA_BUNDLE_HDR_FORMAT] != DWIN_OTA_BUNDLE_FORMAT_VERSION)
    {
        DWIN_LOG_ERR("Bundle format version %d is not supported", header[DWIN_OTA_BUNDLE_HDR_FORMAT]);
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
    }

    bundle->file_count = read_le16(&header[DWIN_OTA_BUNDLE_HDR_FILE_COUNT]);
    bundle->total_size = read_le32(&header[DWIN_OTA_BUNDLE_HDR_TOTAL_SIZE]);
    bundle->table_crc = read_le32(&header[DWIN_OTA_BUNDLE_HDR_CRC]);

    if (bundle->file_count == 0 || bundle->file_count > DWIN_OTA_BUNDLE_MAX_FILES)
    {
        DWIN_LOG_ERR("Bundle carries %d files, 1 to %d are accepted", bundle->file_count, DWIN_OTA_BUNDLE_MAX_FILES);
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
    }

    memcpy(bundle->version, &header[DWIN_OTA_BUNDLE_HDR_VERSION], DWIN_OTA_BUNDLE_VERSION_SIZE);
    bundle->version[DWIN_OTA_BUNDLE_VERSION_SIZE - 1] = '\0';

    /* The table CRC covers the header up to the CRC field, then every entry. */
    bundle->crc = crc32_update(0, header, DWIN_OTA_BUNDLE_HDR_CRC);

    return 0;
}

/**
 * @brief   Read the next file table entry out of the staging buffer.
 * @param   In:   bundle   Pointer to the bundle context.
 */
static void parse_entry(dwin_ota_bundle_t *bundle)
{
    const uint8_t *raw = bundle->staging;
    dwin_ota_bundle_entry_t *entry = &bundle->entries[bundle->file_index];

    entry->file_id = read_le16(&raw[DWIN_OTA_BUNDLE_ENTRY_FILE_ID]);
    entry->offset = read_le32(&raw[DWIN_OTA_BUNDLE_ENTRY_OFFSET]);
    entry->size = read_le32(&raw[DWIN_OTA_BUNDLE_ENTRY_FILE_SIZE]);
    entry->crc32 = read_le32(&raw[DWIN_OTA_BUNDLE_ENTRY_CRC]);

    bundle->crc = crc32_update(bundle->crc, raw, DWIN_OTA_BUNDLE_ENTRY_SIZE);
    bundle->file_index++;
}

/**
 * @brief   Check the complete file table, then pause the display.
 * @param   In:   bundle   Pointer to the bundle context.
 * @note    Nothing has been sent to the display before this point, so a bundle that is
 *          rejected here leaves it running untouched.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int validate_table(dwin_ota_bundle_t *bundle)
{
    uint32_t nor_flash_size = (bundle->cfg.nor_flash_size != 0) ? bundle->cfg.nor_flash_size
                                                                 : DWIN_OTA_NOR_FLASH_SIZE_DEFAULT;
    uint32_t flash_blocks = nor_flash_size / DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE;
    uint32_t data_start = DWIN_OTA_BUNDLE_HEADER_SIZE + ((uint32_t)bundle->file_count * DWIN_OTA_BUNDLE_ENTRY_SIZE);

    if (bundle->crc != bundle->table_crc)
    {
        DWIN_LOG_ERR("Bundle file table is corrupt: CRC %08X != %08X", (unsigned int)bundle->crc,
                     (unsigned int)bundle->table_crc);
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
    }

    if ((bundle->cfg.min_version != NULL) &&
        (!version_is_valid(bundle->version) || version_compare(bundle->version, bundle->cfg.min_version) < 0))
    {
        DWIN_LOG_ERR("Bundle version \"%s\" is older than %s, refusing the downgrade", bundle->version,
                     bundle->cfg.min_version);
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
    }

    bundle->data_size = 0;

    for (uint16_t i = 0; i < bundle->file_count; i++)
    {
        const dwin_ota_bundle_entry_t *entry = &bundle->entries[i];

        /* The files have to come in table order, the stream cannot seek back. */
        if (entry->size == 0 || entry->offset < data_start || entry->offset > bundle->total_size ||
            entry->size > (bundle->total_size - entry->offset))
        {
            DWIN_LOG_ERR("Bundle file %d is misplaced: offset %u, size %u", entry->file_id,
                         (unsigned int)entry->offset, (unsigned int)entry->size);
            return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
        }

        data_start = entry->offset + entry->size;
        bundle->data_size += entry->size;

        /* A file larger than its 256KB slot spills into the next ones, so the room it takes
         * is counted in 32KB blocks. */
        uint32_t first_block = (uint32_t)entry->file_id * DWIN_OTA_PAGES_PER_FILE;
        uint32_t end_block = first_block + ((entry->size + DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE - 1) /
                                            DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE);

        if (end_block > flash_blocks)
        {
            DWIN_LOG_ERR("Bundle file %d of %u bytes does not fit in the NOR flash", entry->file_id,
                         (unsigned int)entry->size);
            return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
        }

        for (uint16_t j = 0; j < i; j++)
        {
            const dwin_ota_bundle_entry_t *other = &bundle->entries[j];
            uint32_t other_first = (uint32_t)other->file_id * DWIN_OTA_PAGES_PER_FILE;
            uint32_t other_end = other_first + ((other->size + DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE - 1) /
                                                DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE);

            if (first_block < other_end && other_first < end_block)
            {
                DWIN_LOG_ERR("Bundle files %d and %d overlap in the NOR flash", other->file_id, entry->file_id);
                return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
            }
        }
    }

    DWIN_LOG_INF("Bundle %s accepted: %d files, %u bytes", bundle->version, bundle->file_count,
                 (unsigned int)bundle->data_size);

    /* Still paused when the table is parsed again after dwin_ota_bundle_restart(). */
    if (!bundle->display_paused)
    {
        if (dwin_pause_display(bundle->cfg.Obj) < 0)
        {
            DWIN_LOG_ERR("Failed to pause the DWIN display");
            return fail(bundle, DWIN_OTA_ERROR_CAUSE_DISPLAY);
        }
        bundle->display_paused = true;
    }

    return 0;
}

/**
 * @brief   Hand the data of the current file over to the display.
 * @param   In:   bundle     Pointer to the bundle context.
 * @param   In:   pData      Pointer to the chunk.
 * @param   In:   len        Length of the chunk, in bytes.
 * @param   Out:  consumed   Number of bytes taken from the chunk.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int write_file_data(dwin_ota_bundle_t *bundle, const uint8_t *pData, size_t len, size_t *consumed)
{
    const dwin_ota_bundle_entry_t *entry = &bundle->entries[bundle->file_index];

    /* Skip whatever sits between two files. */
    if (bundle->stream_pos < entry->offset)
    {
        *consumed = DWIN_OTA_BUNDLE_MIN(len, (size_t)(entry->offset - bundle->stream_pos));
        return 0;
    }

    if (!bundle->file_open)
    {
        const dwin_ota_cfg_t ota_cfg = {
            .Obj = bundle->cfg.Obj,
            .file_id = entry->file_id,
            .file_size = entry->size,
            .nor_flash_size = bundle->cfg.nor_flash_size,
            .callback = NULL, /* the progress is reported over the whole bundle */
        };

        if (dwin_ota_begin(&bundle->ota, &ota_cfg) < 0)
        {
            return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
        }

        bundle->file_open = true;
        bundle->file_remaining = entry->size;
        bundle->crc = 0;
    }

    size_t chunk = DWIN_OTA_BUNDLE_MIN(len, (size_t)bundle->file_remaining);

    bundle->crc = crc32_update(bundle->crc, pData, chunk);

    if (dwin_ota_write(&bundle->ota, pData, chunk) < 0)
    {
        DWIN_LOG_ERR("Failed to write file %d to the display", entry->file_id);
        return fail(bundle, DWIN_OTA_ERROR_CAUSE_DISPLAY);
    }

    bundle->file_remaining -= (uint32_t)chunk;
    bundle->data_written += (uint32_t)chunk;
    *consumed = chunk;

    if (bundle->file_remaining == 0)
    {
        if (dwin_ota_finish(&bundle->ota) < 0)
        {
            DWIN_LOG_ERR("Failed to commit file %d to the NOR flash", entry->file_id);
            return fail(bundle, DWIN_OTA_ERROR_CAUSE_DISPLAY);
        }

        bundle->file_open = false;

        if (bundle->crc != entry->crc32)
        {
            DWIN_LOG_ERR("File %d is corrupt: CRC %08X != %08X", entry->file_id, (unsigned int)bundle->crc,
                         (unsigned int)entry->crc32);
            return fail(bundle, DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
        }

        bundle->file_index++;
        if (bundle->file_index == bundle->file_count)
        {
            bundle->state = BUNDLE_STATE_DONE;
        }

        if (bundle->cfg.callback != NULL)
        {
            const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_FILE_FINISHED};
            bundle->cfg.callback(&evt);
        }
    }

    send_progress(bundle);

    return 0;
}

/**
 * @brief   Report the progress over the whole bundle, at most once per percent.
 * @param   In:   bundle   Pointer to the bundle context.
 */
static void send_progress(dwin_ota_bundle_t *bundle)
{
    if (bundle->cfg.callback == NULL)
    {
        return;
    }

    int progress = dwin_ota_bundle_progress_get(bundle);
    if (progress == bundle->last_progress)
    {
        return;
    }

    bundle->last_progress = progress;

    const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_PROGRESS, .progress = progress};
    bundle->cfg.callback(&evt);
}
