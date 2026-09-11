/***********************************************************************************************************************
 *
 * @file    dwin_ota_downloader_zephyr.c
 * @brief   DWIN over-the-air update over the Zephyr / nRF Connect downloader.
 * @date    01/10/2025
 * @author  Fernando Fontes
 * @note    Platform Target: Zephyr RTOS with the nRF Connect SDK downloader library
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/hash_function.h>
#include <net/downloader.h>

#include "dwin.h"
#include "dwin_ota.h"
#include "dwin_ota_downloader.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#ifndef CONFIG_DWIN_OTA_SOCKET_RETRIES
#define CONFIG_DWIN_OTA_SOCKET_RETRIES 2
#endif

#ifndef CONFIG_DWIN_OTA_SEC_TAG_LIST_SIZE_MAX
#define CONFIG_DWIN_OTA_SEC_TAG_LIST_SIZE_MAX 1
#endif

#ifndef CONFIG_DWIN_OTA_DOWNLOADER_BUF_SIZE
#define CONFIG_DWIN_OTA_DOWNLOADER_BUF_SIZE 2160 /* multiples of 240 bytes */
#endif

/****************************************************** Typedefs ******************************************************/
enum flags_t
{
    /** A single file download is in flight. */
    FLAG_DOWNLOADING,
    /** The next fragment is the first one of a file. */
    FLAG_FIRST_FRAGMENT,
    /** The application asked for the transfer to be cancelled. */
    FLAG_CANCEL,
    /** The whole file list is done, failed or cancelled. Terminal events are
     *  only reported once this is set, so chaining from one file to the next
     *  does not look like the end of the transfer. */
    FLAG_TRANSFER_ENDED,
};

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_REGISTER();

/***************************************************** Variables ******************************************************/
static const char *dl_host;
static const char *dl_file;
static const char *dl_files;
static char *dl_next_file;
static char *dl_strtok_context;

static size_t dl_fragment_size;
static uint8_t dl_pdn_id;
static int dl_sec_tag_list[CONFIG_DWIN_OTA_SEC_TAG_LIST_SIZE_MAX];
static uint8_t dl_sec_tag_count;
static uint8_t dl_number_of_files;
static uint8_t dl_downloaded_files;

static DWIN_Object_t *dl_dwin_object;
static dwin_ota_t dl_ota;

static struct downloader dl;
static char dl_buf[CONFIG_DWIN_OTA_DOWNLOADER_BUF_SIZE];

static int downloader_callback(const struct downloader_evt *event);
static struct downloader_cfg dl_cfg = {
    .callback = downloader_callback,
    .buf = dl_buf,
    .buf_size = sizeof(dl_buf),
};

static struct downloader_host_cfg dl_host_cfg;
static int socket_retries_left;

static bool initialized;
static atomic_t flags;
static enum dwin_ota_error_cause error_state = DWIN_OTA_ERROR_CAUSE_NO_ERROR;
static dwin_ota_callback_t callback;

/*********************************************** Internal Declarations ************************************************/
static void send_evt(enum dwin_ota_evt_id id);
static void send_error_evt(void);
static void set_error_state(enum dwin_ota_error_cause cause);
static bool is_error(void);
static void send_progress(int progress);
static void stopped(void);
static void set_host_and_file(char const *const host, char const *const file);
static int dwin_download(const char *host, const char *file, const int *sec_tag_list,
                         uint8_t sec_tag_count, uint8_t pdn_id, size_t fragment_size);

/***************************************************** Functions ******************************************************/
int dwin_ota_downloader_init(DWIN_Object_t *Obj, dwin_ota_callback_t client_callback)
{
    int err;

    if (Obj == NULL || client_callback == NULL)
    {
        return -EINVAL;
    }

    dl_dwin_object = Obj;
    callback = client_callback;

    if (initialized)
    {
        return 0;
    }

    err = downloader_init(&dl, &dl_cfg);
    if (err != 0)
    {
        return err;
    }

    initialized = true;

    return 0;
}

int dwin_ota_downloader_start(const char *hostname, const char *files, int sec_tag,
                              uint8_t pdn_id, size_t fragment_size)
{
    if (!initialized)
    {
        return -EPERM;
    }

    dl_sec_tag_list[0] = sec_tag;
    dl_sec_tag_count = sec_tag < 0 ? 0 : 1;
    dl_number_of_files = 1;
    for (const char *p = files; *p != '\0'; p++)
    {
        if (*p == ',')
        {
            dl_number_of_files++;
        }
    }
    dl_downloaded_files = 0;
    atomic_clear_bit(&flags, FLAG_CANCEL);
    atomic_clear_bit(&flags, FLAG_TRANSFER_ENDED);
    dl_files = files;
    dl_pdn_id = pdn_id;
    dl_fragment_size = fragment_size;

    // ignore the const qualifier since the caller is not using a const pointer
    dl_next_file = strtok_r((char *)dl_files, ",", &dl_strtok_context);
    if (dl_next_file == NULL)
    {
        DWIN_LOG_ERR("Failed to get first file from comma separated list");
        return -EINVAL;
    }

    return dwin_download(hostname, dl_next_file, dl_sec_tag_list, dl_sec_tag_count, dl_pdn_id, dl_fragment_size);
}

int dwin_ota_downloader_cancel(void)
{
    int err;

    if (!atomic_test_bit(&flags, FLAG_DOWNLOADING))
    {
        /* Download not started, aborted or completed */
        DWIN_LOG_WRN("%s invalid state", __func__);
        return -EAGAIN;
    }

    atomic_set_bit(&flags, FLAG_CANCEL);
    atomic_set_bit(&flags, FLAG_TRANSFER_ENDED);

    err = downloader_cancel(&dl);
    if (err)
    {
        DWIN_LOG_ERR("%s failed to stop download: %d", __func__, err);
        return err;
    }

    while (atomic_test_bit(&flags, FLAG_DOWNLOADING))
    {
        k_msleep(10);
    }

    return err;
}

/************************************************* Internal Functions *************************************************/
static void send_evt(enum dwin_ota_evt_id id)
{
    __ASSERT(id != DWIN_OTA_EVT_PROGRESS, "use send_progress");
    __ASSERT(id != DWIN_OTA_EVT_ERROR, "use send_error_evt");
    const struct dwin_ota_evt evt = {
        .id = id};
    callback(&evt);
}

static void send_error_evt(void)
{
    __ASSERT(error_state != DWIN_OTA_ERROR_CAUSE_NO_ERROR, "use a valid error cause");
    const struct dwin_ota_evt evt = {
        .id = DWIN_OTA_EVT_ERROR,
        .cause = error_state};
    callback(&evt);
}

static void set_error_state(enum dwin_ota_error_cause cause)
{
    error_state = cause;
}

static bool is_error(void)
{
    return error_state != DWIN_OTA_ERROR_CAUSE_NO_ERROR;
}

static void send_progress(int progress)
{
    const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_PROGRESS,
                                     .progress = progress};
    callback(&evt);
}

static void stopped(void)
{
    atomic_clear_bit(&flags, FLAG_DOWNLOADING);
    if (is_error())
    {
        send_error_evt();
    }
    else if (atomic_test_and_clear_bit(&flags, FLAG_CANCEL))
    {
        send_evt(DWIN_OTA_EVT_CANCELLED);
    }
    else
    {
        send_evt(DWIN_OTA_EVT_FINISHED);
    }
}

static int downloader_callback(const struct downloader_evt *event)
{
    size_t offset;
    size_t file_size;
    int err;

    if (event == NULL)
    {
        return -EINVAL;
    }

    switch (event->id)
    {
    case DOWNLOADER_EVT_FRAGMENT:
    {
        if (atomic_test_and_clear_bit(&flags, FLAG_FIRST_FRAGMENT))
        {
            err = downloader_file_size_get(&dl, &file_size);
            if (err != 0)
            {
                DWIN_LOG_DBG("file_size_get err: %d", err);
                set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
                goto error_and_close;
            }

            /* Pause the display once, before the very first file. */
            if (dl_downloaded_files == 0)
            {
                if (dwin_pause_display(dl_dwin_object) < 0)
                {
                    DWIN_LOG_ERR("Failed to pause DWIN display");
                    set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
                    goto error_and_close;
                }
            }

            const dwin_ota_cfg_t ota_cfg = {
                .Obj = dl_dwin_object,
                .file_id = dwin_ota_file_id_from_name(dl_file),
                .file_size = (uint32_t)file_size,
                .callback = NULL, /* progress is aggregated over the whole file list below */
            };

            if (dwin_ota_begin(&dl_ota, &ota_cfg) < 0)
            {
                set_error_state(DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
                goto error_and_close;
            }
        }

        if (dwin_ota_write(&dl_ota, (const uint8_t *)event->fragment.buf, event->fragment.len) < 0)
        {
            DWIN_LOG_ERR("Failed to write the fragment to the display");
            set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
            goto error_and_close;
        }

        // Get the downloaded size
        err = downloader_downloaded_size_get(&dl, &offset);
        if (err != 0)
        {
            DWIN_LOG_DBG("unable to get downloaded size err: %d", err);
            set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
            goto error_and_close;
        }

        int progress = dwin_ota_progress_get(&dl_ota);
        send_progress((dl_downloaded_files * 100 / dl_number_of_files) + (progress / dl_number_of_files));

        DWIN_LOG_DBG("Progress: %u bytes", (unsigned int)offset);
        break;
    }

    case DOWNLOADER_EVT_DONE:
        DWIN_LOG_DBG("DOWNLOADER_EVT_DONE");

        if (dwin_ota_finish(&dl_ota) < 0)
        {
            DWIN_LOG_ERR("Failed to commit the last block to the NOR flash");
            set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
            goto error_and_close;
        }

        dl_downloaded_files++;
        send_evt(DWIN_OTA_EVT_FILE_FINISHED);

        dl_next_file = strtok_r(NULL, ",", &dl_strtok_context);
        if (dl_next_file != NULL)
        {
            /* Release the per file guard so the next download can be armed. The
             * transfer itself is still running, so FLAG_TRANSFER_ENDED stays clear
             * and the DOWNLOADER_EVT_STOPPED that follows is ignored. */
            atomic_clear_bit(&flags, FLAG_DOWNLOADING);
            err = dwin_download(dl_host, dl_next_file, dl_sec_tag_list, dl_sec_tag_count, dl_pdn_id, dl_fragment_size);
            if (err != 0)
            {
                DWIN_LOG_ERR("Failed to start next download");
                set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
                goto error_and_close;
            }
            break;
        }

        if (dwin_reset_display(dl_dwin_object) < 0)
        {
            DWIN_LOG_ERR("Failed to reset DWIN display");
            set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
            goto error_and_close;
        }

        send_progress(100);
        atomic_set_bit(&flags, FLAG_TRANSFER_ENDED);
        break;

    case DOWNLOADER_EVT_ERROR:
        /* In case of socket errors we can return 0 to retry/continue,
         * or non-zero to stop
         */
        if ((socket_retries_left) && ((event->error == -ECONNRESET) || (event->error == -EAGAIN)))
        {
            DWIN_LOG_WRN("Download socket error. %d retries left...",
                         socket_retries_left);
            socket_retries_left--;
            /* Fall through and return 0 below to tell
             * downloader to retry
             */
            break;
        }
        DWIN_LOG_ERR("Downloader error event %d", event->error);
        switch (event->error)
        {

        case -ECONNABORTED:
        case -ECONNREFUSED:
        case -EHOSTUNREACH:
        case -ENETDOWN:
            set_error_state(DWIN_OTA_ERROR_CAUSE_CONNECT_FAILED);
            break;
        case -ECONNRESET:
        case -ETIMEDOUT:
        case -EHOSTDOWN:
        case -EBADMSG:
        case -ERANGE:
        case -E2BIG:
            set_error_state(DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED);
            break;
        case -EPROTONOSUPPORT:
            set_error_state(DWIN_OTA_ERROR_CAUSE_PROTO_NOT_SUPPORTED);
            break;
        case -EINVAL:
        case -EAFNOSUPPORT:
            set_error_state(DWIN_OTA_ERROR_CAUSE_INVALID_CONFIGURATION);
            break;
        default:
            set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
            break;
        }
        goto error_and_close;

    case DOWNLOADER_EVT_STOPPED:
        /* The downloader also stops between two files of the same transfer. */
        if (!atomic_test_bit(&flags, FLAG_TRANSFER_ENDED))
        {
            break;
        }
        stopped();
        break;

    case DOWNLOADER_EVT_DEINITIALIZED:
    default:
        break;
    }

    return 0;

error_and_close:
    dwin_ota_abort(&dl_ota);
    atomic_set_bit(&flags, FLAG_TRANSFER_ENDED);
    return -1;
}

static void set_host_and_file(char const *const host, char const *const file)
{
    DWIN_LOG_INF("Setting host and file: %s, %s", host, file);

    dl_host = host;
    dl_file = file;

    DWIN_LOG_INF("DGUS file id: %d", dwin_ota_file_id_from_name(file));
}

static int dwin_download(const char *host, const char *file,
                         const int *sec_tag_list, uint8_t sec_tag_count, uint8_t pdn_id, size_t fragment_size)
{
    if (host == NULL || file == NULL || callback == NULL)
    {
        return -EINVAL;
    }

    if (atomic_test_and_set_bit(&flags, FLAG_DOWNLOADING))
    {
        return -EALREADY;
    }

    int err;
    static int sec_tag_list_copy[CONFIG_DWIN_OTA_SEC_TAG_LIST_SIZE_MAX];

    dl_host_cfg.pdn_id = pdn_id;
    dl_host_cfg.range_override = fragment_size;
    dl_host_cfg.family = AF_INET;
    // Keep connection if there are more than one file to download and we are not on the last file
    if ((dl_number_of_files > 1) && (dl_downloaded_files < dl_number_of_files - 1))
    {
        dl_host_cfg.keep_connection = true;
    }
    else
    {
        dl_host_cfg.keep_connection = false;
    }

    if (sec_tag_count > ARRAY_SIZE(sec_tag_list_copy))
    {
        atomic_clear_bit(&flags, FLAG_DOWNLOADING);
        return -E2BIG;
    }

    set_error_state(DWIN_OTA_ERROR_CAUSE_NO_ERROR);

    set_host_and_file(host, file);

    if ((sec_tag_list != NULL) && (sec_tag_count > 0))
    {
        memcpy(sec_tag_list_copy, sec_tag_list, sec_tag_count * sizeof(sec_tag_list[0]));

        dl_host_cfg.sec_tag_count = sec_tag_count;
        dl_host_cfg.sec_tag_list = sec_tag_list_copy;
    }

    socket_retries_left = CONFIG_DWIN_OTA_SOCKET_RETRIES;

    atomic_set_bit(&flags, FLAG_FIRST_FRAGMENT);

    err = downloader_get_with_host_and_file(&dl, &dl_host_cfg, dl_host, dl_file, 0);
    if (err != 0)
    {
        atomic_clear_bit(&flags, FLAG_DOWNLOADING);
        (void)downloader_cancel(&dl);
        return err;
    }

    return 0;
}
