/***********************************************************************************************************************
 *
 * @file    main.c
 * @brief   DWIN SDK over-the-air display update example for ESP-IDF.
 * @date    11/09/2026
 * @author  Fernando Fontes
 * @note    Platform Target: ESP32 family (ESP-IDF v5.x)
 *
 * @details Downloads a .dwpk bundle over HTTP and rewrites the DGUS files it carries
 *          into the display NOR flash. Expect roughly 6 seconds per 32KB block, so a
 *          256KB .icl file takes about 50 seconds. Build the bundle with:
 *
 *              tools/dwin_pack.py pack DWIN_SET --version 1.4.0 --last 22 -o hmi-1.4.0.dwpk
 *
 * SPDX-License-Identifier: MIT
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "dwin.h"
#include "dwin_port.h"
#include "dwin_ota.h"
#include "dwin_ota_http.h"

/****************************************************** Defines *******************************************************/
#define DISPLAY_UART_PORT UART_NUM_1
#define DISPLAY_TX_PIN 17
#define DISPLAY_RX_PIN 16
#define DISPLAY_BAUD_RATE 115200

#define WIFI_SSID "my-ssid"
#define WIFI_PASSWORD "my-password"
#define WIFI_CONNECTED_BIT BIT0

/* The bundle with the DGUS files to refresh. Plain HTTP is only good for a test against a machine on
 * your desk: in a product use an https:// URL and set .cert_pem, a bundle is not signed. */
#define OTA_BUNDLE_URL "http://192.168.1.10:8000/display/hmi-1.4.0.dwpk"

/***************************************************** Constants ******************************************************/
static const char *TAG = "example";

/***************************************************** Variables ******************************************************/
static DWIN_Object_t display;
static EventGroupHandle_t wifi_event_group;

/*********************************************** Internal Declarations ************************************************/
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void wifi_connect(void);
static void ota_event_handler(const struct dwin_ota_evt *evt);

/***************************************************** Functions ******************************************************/
void app_main(void)
{
    const dwin_port_cfg_t port_cfg = {
        .uart_port = DISPLAY_UART_PORT,
        .tx_pin = DISPLAY_TX_PIN,
        .rx_pin = DISPLAY_RX_PIN,
        .baud_rate = DISPLAY_BAUD_RATE,
        .rx_buffer_size = 1024,
        .tx_buffer_size = 0,
    };

    ESP_ERROR_CHECK(nvs_flash_init());

    if (dwin_port_init(&display, &port_cfg) < 0)
    {
        ESP_LOGE(TAG, "Failed to bring the display up");
        return;
    }

    /* The display reboots after dwin_init(), give it time to come back. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    wifi_connect();

    if (dwin_ota_http_init(ota_event_handler) < 0)
    {
        ESP_LOGE(TAG, "Failed to initialize the OTA client");
        return;
    }

    const dwin_ota_http_cfg_t ota_cfg = {
        .Obj = &display,
        .url = OTA_BUNDLE_URL,
        .timeout_ms = 15000,
        .retries = 2,
    };

    if (dwin_ota_http_start(&ota_cfg) < 0)
    {
        ESP_LOGE(TAG, "Failed to start the display update");
        return;
    }

    while (dwin_ota_http_is_running())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Display update sequence done");
}

/************************************************* Internal Functions *************************************************/
static void ota_event_handler(const struct dwin_ota_evt *evt)
{
    switch (evt->id)
    {
    case DWIN_OTA_EVT_STARTED:
        ESP_LOGI(TAG, "Display update started");
        break;
    case DWIN_OTA_EVT_PROGRESS:
        ESP_LOGI(TAG, "Display update progress: %d%%", evt->progress);
        break;
    case DWIN_OTA_EVT_FILE_FINISHED:
        ESP_LOGI(TAG, "One file committed to the NOR flash");
        break;
    case DWIN_OTA_EVT_FINISHED:
        ESP_LOGI(TAG, "Display update finished");
        break;
    case DWIN_OTA_EVT_CANCELLED:
        ESP_LOGW(TAG, "Display update cancelled");
        break;
    case DWIN_OTA_EVT_ERROR:
        ESP_LOGE(TAG, "Display update failed, cause %d", evt->cause);
        break;
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_connect(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected");
}
