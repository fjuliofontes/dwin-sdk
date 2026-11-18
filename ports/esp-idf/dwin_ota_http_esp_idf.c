/***********************************************************************************************************************
 *
 * @file    dwin_ota_http_esp_idf.c
 * @brief   DWIN over-the-air update over HTTP(S) for ESP-IDF.
 * @date    11/09/2026
 * @author  Fernando Fontes
 * @note    Platform Target: ESP32 family (ESP-IDF v5.x)
 *
 * SPDX-License-Identifier: MIT
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"

#include "dwin.h"
#include "dwin_ota.h"
#include "dwin_ota_bundle.h"
#include "dwin_ota_http.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#define DWIN_OTA_HTTP_CHUNK_SIZE 1024
#define DWIN_OTA_HTTP_RANGE_SIZE 32

#define DWIN_OTA_HTTP_DEFAULT_TIMEOUT_MS 10000
#define DWIN_OTA_HTTP_DEFAULT_RETRIES 2
#define DWIN_OTA_HTTP_DEFAULT_PRIORITY 5
#define DWIN_OTA_HTTP_DEFAULT_STACK_SIZE 8192

#define DWIN_OTA_HTTP_STATUS_OK 200
#define DWIN_OTA_HTTP_STATUS_PARTIAL_CONTENT 206

/****************************************************** Typedefs ******************************************************/
enum download_result
{
    /** The whole bundle is in the display NOR flash. */
    DOWNLOAD_DONE,
    /** The connection broke, the download can be resumed. */
    DOWNLOAD_RETRY,
    /** The bundle or the display failed, or the transfer was cancelled. */
    DOWNLOAD_ABORT,
};

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_DECLARE();

/***************************************************** Variables ******************************************************/
static dwin_ota_http_cfg_t ota_cfg;
static dwin_ota_callback_t callback;
static dwin_ota_bundle_t ota_bundle;

static uint8_t ota_chunk[DWIN_OTA_HTTP_CHUNK_SIZE];

static volatile bool ota_running;
static volatile bool ota_cancel_request;
static enum dwin_ota_error_cause error_state = DWIN_OTA_ERROR_CAUSE_NO_ERROR;

/*********************************************** Internal Declarations ************************************************/
static void send_evt(enum dwin_ota_evt_id id);
static void send_error_evt(void);
static void set_error_state(enum dwin_ota_error_cause cause);
static bool is_error(void);
static enum dwin_ota_error_cause http_error_cause_get(esp_err_t err);
static enum download_result download_bundle(uint32_t offset);
static void dwin_ota_http_task(void *arg);

/***************************************************** Functions ******************************************************/
int dwin_ota_http_init(dwin_ota_callback_t client_callback)
{
    if (client_callback == NULL)
    {
        return -1;
    }

    callback = client_callback;

    return 0;
}

int dwin_ota_http_start(const dwin_ota_http_cfg_t *cfg)
{
    if (cfg == NULL || cfg->Obj == NULL || cfg->url == NULL || callback == NULL)
    {
        return -1;
    }

    if (ota_running)
    {
        DWIN_LOG_WRN("%s invalid state, a transfer is already running", __func__);
        return -1;
    }

    ota_cfg = *cfg;

    ota_cancel_request = false;
    ota_running = true;
    set_error_state(DWIN_OTA_ERROR_CAUSE_NO_ERROR);

    int priority = (cfg->task_priority > 0) ? cfg->task_priority : DWIN_OTA_HTTP_DEFAULT_PRIORITY;
    int stack_size = (cfg->task_stack_size > 0) ? cfg->task_stack_size : DWIN_OTA_HTTP_DEFAULT_STACK_SIZE;

    if (xTaskCreate(dwin_ota_http_task, "dwin_ota", stack_size, NULL, priority, NULL) != pdPASS)
    {
        DWIN_LOG_ERR("Failed to create the DWIN OTA task");
        ota_running = false;
        return -1;
    }

    return 0;
}

int dwin_ota_http_cancel(void)
{
    if (!ota_running)
    {
        DWIN_LOG_WRN("%s invalid state", __func__);
        return -1;
    }

    ota_cancel_request = true;

    return 0;
}

bool dwin_ota_http_is_running(void)
{
    return ota_running;
}

/************************************************* Internal Functions *************************************************/
static void send_evt(enum dwin_ota_evt_id id)
{
    const struct dwin_ota_evt evt = {
        .id = id};
    callback(&evt);
}

static void send_error_evt(void)
{
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

static enum dwin_ota_error_cause http_error_cause_get(esp_err_t err)
{
    switch (err)
    {
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_CONNECTING:
        return DWIN_OTA_ERROR_CAUSE_CONNECT_FAILED;
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_FETCH_HEADER:
        return DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED;
    case ESP_ERR_INVALID_ARG:
        return DWIN_OTA_ERROR_CAUSE_INVALID_CONFIGURATION;
    default:
        return DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED;
    }
}

/**
 * @brief   Download the bundle and stream it into the display NOR flash.
 * @param   In:   offset   Bundle offset to start from, 0 for the whole bundle.
 * @note    The error state is set whenever the result is not DOWNLOAD_DONE.
 * @retval  Returns what to do next with the transfer.
 */
static enum download_result download_bundle(uint32_t offset)
{
    enum download_result result = DOWNLOAD_RETRY;
    int status_code;
    esp_err_t err;
    esp_http_client_handle_t client = NULL;
    char range[DWIN_OTA_HTTP_RANGE_SIZE];

    DWIN_LOG_INF("Downloading %s from byte %u", ota_cfg.url, (unsigned int)offset);

    esp_http_client_config_t client_cfg = {
        .url = ota_cfg.url,
        .cert_pem = ota_cfg.cert_pem,
        .skip_cert_common_name_check = ota_cfg.skip_cert_common_name_check,
        .timeout_ms = (ota_cfg.timeout_ms > 0) ? ota_cfg.timeout_ms : DWIN_OTA_HTTP_DEFAULT_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    client = esp_http_client_init(&client_cfg);
    if (client == NULL)
    {
        DWIN_LOG_ERR("Failed to initialize the HTTP client");
        set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
        return DOWNLOAD_ABORT;
    }

    if (offset > 0)
    {
        snprintf(range, sizeof(range), "bytes=%u-", (unsigned int)offset);
        esp_http_client_set_header(client, "Range", range);
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        DWIN_LOG_ERR("Failed to open the HTTP connection: %s", esp_err_to_name(err));
        set_error_state(http_error_cause_get(err));
        goto cleanup_and_return;
    }

    if (esp_http_client_fetch_headers(client) < 0)
    {
        DWIN_LOG_ERR("Failed to fetch the HTTP headers");
        set_error_state(DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED);
        goto cleanup_and_return;
    }

    status_code = esp_http_client_get_status_code(client);

    if ((offset > 0) && (status_code == DWIN_OTA_HTTP_STATUS_OK))
    {
        /* The server ignored the Range header and sends the bundle from its first byte. */
        DWIN_LOG_WRN("Server does not resume downloads, starting the bundle over");
        if (dwin_ota_bundle_restart(&ota_bundle) < 0)
        {
            set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
            result = DOWNLOAD_ABORT;
            goto cleanup_and_return;
        }
    }
    else if (status_code != ((offset > 0) ? DWIN_OTA_HTTP_STATUS_PARTIAL_CONTENT : DWIN_OTA_HTTP_STATUS_OK))
    {
        DWIN_LOG_ERR("Server answered with status %d", status_code);
        set_error_state(DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED);
        goto cleanup_and_return;
    }

    while (!dwin_ota_bundle_is_complete(&ota_bundle))
    {
        if (ota_cancel_request)
        {
            DWIN_LOG_WRN("Transfer cancelled by the application");
            result = DOWNLOAD_ABORT;
            goto cleanup_and_return;
        }

        int read_len = esp_http_client_read(client, (char *)ota_chunk, sizeof(ota_chunk));
        if (read_len <= 0)
        {
            DWIN_LOG_ERR("HTTP stream ended before the end of the bundle: %d", read_len);
            set_error_state(DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED);
            goto cleanup_and_return;
        }

        if (dwin_ota_bundle_write(&ota_bundle, ota_chunk, (size_t)read_len) < 0)
        {
            DWIN_LOG_ERR("Failed to write the chunk to the display");
            set_error_state(dwin_ota_bundle_error_cause_get(&ota_bundle));
            result = DOWNLOAD_ABORT;
            goto cleanup_and_return;
        }
    }

    result = DOWNLOAD_DONE;

cleanup_and_return:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return result;
}

static void dwin_ota_http_task(void *arg)
{
    int retries_left = (ota_cfg.retries > 0) ? ota_cfg.retries : DWIN_OTA_HTTP_DEFAULT_RETRIES;
    uint32_t offset = 0;

    (void)arg;

    send_evt(DWIN_OTA_EVT_STARTED);

    /* The bundle reports the progress and every finished file straight to the application. */
    const dwin_ota_bundle_cfg_t bundle_cfg = {
        .Obj = ota_cfg.Obj,
        .nor_flash_size = ota_cfg.nor_flash_size,
        .min_version = ota_cfg.min_version,
        .callback = callback,
    };

    if (dwin_ota_bundle_begin(&ota_bundle, &bundle_cfg) < 0)
    {
        set_error_state(DWIN_OTA_ERROR_CAUSE_INVALID_CONFIGURATION);
        goto stop_and_report;
    }

    for (;;)
    {
        enum download_result result = download_bundle(offset);

        if (result == DOWNLOAD_DONE)
        {
            break;
        }

        if (result == DOWNLOAD_ABORT || ota_cancel_request || retries_left == 0)
        {
            goto stop_and_report;
        }

        DWIN_LOG_WRN("Download of the bundle failed, %d retries left...", retries_left);
        retries_left--;

        /* Pick the download up at the file it stopped in, the ones before are already written. */
        if (dwin_ota_bundle_rewind(&ota_bundle, &offset) < 0)
        {
            set_error_state(DWIN_OTA_ERROR_CAUSE_INTERNAL);
            goto stop_and_report;
        }

        set_error_state(DWIN_OTA_ERROR_CAUSE_NO_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Resets the display, which boots into the new files. */
    if (dwin_ota_bundle_finish(&ota_bundle) < 0)
    {
        set_error_state(dwin_ota_bundle_error_cause_get(&ota_bundle));
    }

stop_and_report:
    if (is_error() || ota_cancel_request)
    {
        /* Also brings a paused display back. */
        dwin_ota_bundle_abort(&ota_bundle);
    }

    ota_running = false;

    if (is_error())
    {
        send_error_evt();
    }
    else if (ota_cancel_request)
    {
        send_evt(DWIN_OTA_EVT_CANCELLED);
    }
    else
    {
        send_evt(DWIN_OTA_EVT_FINISHED);
    }

    vTaskDelete(NULL);
}
