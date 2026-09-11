/***********************************************************************************************************************
 *
 * @file    dwin_ota_http_esp_idf.c
 * @brief   DWIN over-the-air update over HTTP(S) for ESP-IDF.
 * @date    11/09/2026
 * @author  Fernando Fontes
 * @note    Platform Target: ESP32 family (ESP-IDF v5.x)
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"

#include "dwin.h"
#include "dwin_ota.h"
#include "dwin_ota_http.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#define DWIN_OTA_HTTP_URL_SIZE 256
#define DWIN_OTA_HTTP_FILE_LIST_SIZE 256
#define DWIN_OTA_HTTP_CHUNK_SIZE 1024

#define DWIN_OTA_HTTP_DEFAULT_TIMEOUT_MS 10000
#define DWIN_OTA_HTTP_DEFAULT_RETRIES 2
#define DWIN_OTA_HTTP_DEFAULT_PRIORITY 5
#define DWIN_OTA_HTTP_DEFAULT_STACK_SIZE 8192

/****************************************************** Typedefs ******************************************************/

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_REGISTER();

/***************************************************** Variables ******************************************************/
static dwin_ota_http_cfg_t ota_cfg;
static dwin_ota_callback_t callback;
static dwin_ota_t ota;

static char ota_file_list[DWIN_OTA_HTTP_FILE_LIST_SIZE];
static char ota_url[DWIN_OTA_HTTP_URL_SIZE];
static uint8_t ota_chunk[DWIN_OTA_HTTP_CHUNK_SIZE];

static uint8_t ota_number_of_files;
static uint8_t ota_downloaded_files;

static volatile bool ota_running;
static volatile bool ota_cancel_request;
static enum dwin_ota_error_cause error_state = DWIN_OTA_ERROR_CAUSE_NO_ERROR;

/*********************************************** Internal Declarations ************************************************/
static void send_evt(enum dwin_ota_evt_id id);
static void send_error_evt(void);
static void set_error_state(enum dwin_ota_error_cause cause);
static bool is_error(void);
static void send_progress(int progress);
static uint16_t count_occurrences(const char *s, char c);
static enum dwin_ota_error_cause http_error_cause_get(esp_err_t err);
static int download_file(const char *file_name);
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
    if (cfg == NULL || cfg->Obj == NULL || cfg->base_url == NULL || cfg->files == NULL || callback == NULL)
    {
        return -1;
    }

    if (ota_running)
    {
        DWIN_LOG_WRN("%s invalid state, a transfer is already running", __func__);
        return -1;
    }

    if (strlen(cfg->files) >= sizeof(ota_file_list))
    {
        DWIN_LOG_ERR("File list is longer than %d bytes", (int)sizeof(ota_file_list));
        return -1;
    }

    ota_cfg = *cfg;

    /* Work on our own copy, strtok_r() rewrites the string in place. */
    strncpy(ota_file_list, cfg->files, sizeof(ota_file_list) - 1);
    ota_file_list[sizeof(ota_file_list) - 1] = '\0';

    ota_number_of_files = count_occurrences(ota_file_list, ',') + 1;
    ota_downloaded_files = 0;
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

static void send_progress(int progress)
{
    const struct dwin_ota_evt evt = {.id = DWIN_OTA_EVT_PROGRESS,
                                     .progress = progress};
    callback(&evt);
}

static uint16_t count_occurrences(const char *s, char c)
{
    uint16_t count = 0;
    while (*s)
    {
        if (*s == c)
            count++;
        s++;
    }
    return count;
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
 * @brief   Download one DGUS file and stream it into the display NOR flash.
 * @param   In:   file_name   File to fetch, appended to the configured base URL.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int download_file(const char *file_name)
{
    int ret = 0;
    int64_t content_length;
    int status_code;
    esp_err_t err;
    esp_http_client_handle_t client = NULL;

    snprintf(ota_url, sizeof(ota_url), "%s/%s", ota_cfg.base_url, file_name);
    DWIN_LOG_INF("Downloading %s", ota_url);

    esp_http_client_config_t client_cfg = {
        .url = ota_url,
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
        return -1;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        DWIN_LOG_ERR("Failed to open the HTTP connection: %s", esp_err_to_name(err));
        set_error_state(http_error_cause_get(err));
        ret = -1;
        goto cleanup_and_return;
    }

    content_length = esp_http_client_fetch_headers(client);
    status_code = esp_http_client_get_status_code(client);

    if (status_code != 200)
    {
        DWIN_LOG_ERR("Server answered with status %d", status_code);
        set_error_state(DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED);
        ret = -1;
        goto cleanup_and_return;
    }

    if (content_length <= 0)
    {
        DWIN_LOG_ERR("Server did not report a usable content length: %lld", (long long)content_length);
        set_error_state(DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
        ret = -1;
        goto cleanup_and_return;
    }

    const dwin_ota_cfg_t ota_file_cfg = {
        .Obj = ota_cfg.Obj,
        .file_id = dwin_ota_file_id_from_name(file_name),
        .file_size = (uint32_t)content_length,
        .callback = NULL, /* progress is aggregated over the whole file list below */
    };

    if (dwin_ota_begin(&ota, &ota_file_cfg) < 0)
    {
        set_error_state(DWIN_OTA_ERROR_CAUSE_INVALID_UPDATE);
        ret = -1;
        goto cleanup_and_return;
    }

    for (;;)
    {
        if (ota_cancel_request)
        {
            DWIN_LOG_WRN("Transfer cancelled by the application");
            dwin_ota_abort(&ota);
            ret = -1;
            goto cleanup_and_return;
        }

        int read_len = esp_http_client_read(client, (char *)ota_chunk, sizeof(ota_chunk));
        if (read_len < 0)
        {
            DWIN_LOG_ERR("Failed to read from the HTTP stream");
            set_error_state(DWIN_OTA_ERROR_CAUSE_DOWNLOAD_FAILED);
            dwin_ota_abort(&ota);
            ret = -1;
            goto cleanup_and_return;
        }

        if (read_len == 0)
        {
            break;
        }

        if (dwin_ota_write(&ota, ota_chunk, (size_t)read_len) < 0)
        {
            DWIN_LOG_ERR("Failed to write the chunk to the display");
            set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
            dwin_ota_abort(&ota);
            ret = -1;
            goto cleanup_and_return;
        }

        /* Aggregate the per file progress over the whole list of files. */
        int progress = dwin_ota_progress_get(&ota);
        send_progress((ota_downloaded_files * 100 / ota_number_of_files) + (progress / ota_number_of_files));
    }

    if (dwin_ota_finish(&ota) < 0)
    {
        DWIN_LOG_ERR("Failed to commit the last block to the NOR flash");
        set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
        ret = -1;
        goto cleanup_and_return;
    }

    DWIN_LOG_INF("%s written to the display", file_name);

cleanup_and_return:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return ret;
}

static void dwin_ota_http_task(void *arg)
{
    char *file_name;
    char *strtok_context = NULL;
    int retries_left;

    (void)arg;

    send_evt(DWIN_OTA_EVT_STARTED);

    if (dwin_pause_display(ota_cfg.Obj) < 0)
    {
        DWIN_LOG_ERR("Failed to pause the DWIN display");
        set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
        goto stop_and_report;
    }

    file_name = strtok_r(ota_file_list, ",", &strtok_context);
    if (file_name == NULL)
    {
        DWIN_LOG_ERR("Failed to get the first file from the comma separated list");
        set_error_state(DWIN_OTA_ERROR_CAUSE_INVALID_CONFIGURATION);
        goto stop_and_report;
    }

    while (file_name != NULL)
    {
        retries_left = (ota_cfg.retries > 0) ? ota_cfg.retries : DWIN_OTA_HTTP_DEFAULT_RETRIES;

        while (download_file(file_name) < 0)
        {
            if (ota_cancel_request || retries_left == 0)
            {
                goto stop_and_report;
            }

            DWIN_LOG_WRN("Download of %s failed, %d retries left...", file_name, retries_left);
            retries_left--;
            set_error_state(DWIN_OTA_ERROR_CAUSE_NO_ERROR);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ota_downloaded_files++;
        send_evt(DWIN_OTA_EVT_FILE_FINISHED);

        file_name = strtok_r(NULL, ",", &strtok_context);
    }

    if (dwin_reset_display(ota_cfg.Obj) < 0)
    {
        DWIN_LOG_ERR("Failed to reset the DWIN display");
        set_error_state(DWIN_OTA_ERROR_CAUSE_DISPLAY);
        goto stop_and_report;
    }

    send_progress(100);

stop_and_report:
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
