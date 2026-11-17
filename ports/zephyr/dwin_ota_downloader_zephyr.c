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
#include "dwin_ota_bundle.h"
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
#define CONFIG_DWIN_OTA_DOWNLOADER_BUF_SIZE 2160
#endif

/****************************************************** Typedefs ******************************************************/
enum flags_t
{
    /** A bundle download is in flight. */
    FLAG_DOWNLOADING,
    /** The application asked for the transfer to be cancelled. */
    FLAG_CANCEL,
};

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_REGISTER();

/***************************************************** Variables ******************************************************/
static DWIN_Object_t *dl_dwin_object;
static dwin_ota_bundle_t dl_bundle;

static struct downloader dl;
static char dl_buf[CONFIG_DWIN_OTA_DOWNLOADER_BUF_SIZE];

static int downloader_callback(const struct downloader_evt *event);
static struct downloader_cfg dl_cfg = {
    .callback = downloader_callback,
    .buf = dl_buf,
    .buf_size = sizeof(dl_buf),
};

static struct downloader_host_cfg dl_host_cfg;
static int dl_sec_tag_list[CONFIG_DWIN_OTA_SEC_TAG_LIST_SIZE_MAX];
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
static void stopped(void);

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

int dwin_ota_downloader_start(const char *hostname, const char *file, const char *min_version, int sec_tag,
                              uint8_t pdn_id, size_t fragment_size)
{
    int err;

    if (!initialized)
    {
        return -EPERM;
    }

    if (hostname == NULL || file == NULL)
    {
        return -EINVAL;
    }

    if (atomic_test_and_set_bit(&flags, FLAG_DOWNLOADING))
    {
        return -EALREADY;
    }

    /* The bundle reports the progress and every finished file straight to the application. */
    const dwin_ota_bundle_cfg_t bundle_cfg = {
        .Obj = dl_dwin_object,
        .min_version = min_version,
        .callback = callback,
    };

    if (dwin_ota_bundle_begin(&dl_bundle, &bundle_cfg) < 0)
    {
        atomic_clear_bit(&flags, FLAG_DOWNLOADING);
        return -EINVAL;
    }

    atomic_clear_bit(&flags, FLAG_CANCEL);
    set_error_state(DWIN_OTA_ERROR_CAUSE_NO_ERROR);
    socket_retries_left = CONFIG_DWIN_OTA_SOCKET_RETRIES;

    dl_sec_tag_list[0] = sec_tag;

    dl_host_cfg.pdn_id = pdn_id;
    dl_host_cfg.range_override = fragment_size;
    dl_host_cfg.family = AF_INET;
    dl_host_cfg.keep_connection = false;
    dl_host_cfg.sec_tag_list = dl_sec_tag_list;
    dl_host_cfg.sec_tag_count = (sec_tag < 0) ? 0 : 1;

    DWIN_LOG_INF("Downloading bundle %s from %s", file, hostname);

    send_evt(DWIN_OTA_EVT_STARTED);

    err = downloader_get_with_host_and_file(&dl, &dl_host_cfg, hostname, file, 0);
    if (err != 0)
    {
        atomic_clear_bit(&flags, FLAG_DOWNLOADING);
        (void)downloader_cancel(&dl);
        return err;
    }

    return 0;
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

static void stopped(void)
{
    /* Nothing to report when the download never started. */
    if (!atomic_test_bit(&flags, FLAG_DOWNLOADING))
    {
        return;
    }

    bool cancelled = atomic_test_and_clear_bit(&flags, FLAG_CANCEL);

    if (is_error() || cancelled)
    {
        /* Also brings a paused display back. */
        dwin_ota_bundle_abort(&dl_bundle);
    }

    atomic_clear_bit(&flags, FLAG_DOWNLOADING);

    if (is_error())
    {
        send_error_evt();
    }
    else if (cancelled)
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
    if (event == NULL)
    {
        return -EINVAL;
    }

    switch (event->id)
    {
    case DOWNLOADER_EVT_FRAGMENT:
        /* The downloader resumes a broken connection from where it stopped, so the fragments
         * always come as one continuous stream. */
        if (dwin_ota_bundle_write(&dl_bundle, (const uint8_t *)event->fragment.buf, event->fragment.len) < 0)
        {
            DWIN_LOG_ERR("Failed to write the fragment to the display");
            set_error_state(dwin_ota_bundle_error_cause_get(&dl_bundle));
            goto error_and_close;
        }
        break;

    case DOWNLOADER_EVT_DONE:
        DWIN_LOG_DBG("DOWNLOADER_EVT_DONE");

        /* Resets the display, which boots into the new files. */
        if (dwin_ota_bundle_finish(&dl_bundle) < 0)
        {
            DWIN_LOG_ERR("Failed to close the bundle transfer");
            set_error_state(dwin_ota_bundle_error_cause_get(&dl_bundle));
            goto error_and_close;
        }
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
        /* Always comes last, whether the download finished, failed or was cancelled. */
        stopped();
        break;

    case DOWNLOADER_EVT_DEINITIALIZED:
    default:
        break;
    }

    return 0;

error_and_close:
    return -1;
}
