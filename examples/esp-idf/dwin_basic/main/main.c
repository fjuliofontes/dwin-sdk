/***********************************************************************************************************************
 *
 * @file    main.c
 * @brief   DWIN SDK basic example for ESP-IDF.
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
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "dwin.h"
#include "dwin_port.h"

/****************************************************** Defines *******************************************************/
#define DISPLAY_UART_PORT UART_NUM_1
#define DISPLAY_TX_PIN 17
#define DISPLAY_RX_PIN 16
#define DISPLAY_BAUD_RATE 115200

/* VP addresses of the DGUS project. Replace with the ones from your own project. */
#define VP_COUNTER 0x1000
#define VP_TITLE 0x2000
#define SP_TITLE 0x5000

/***************************************************** Constants ******************************************************/
static const char *TAG = "example";

/***************************************************** Variables ******************************************************/
static DWIN_Object_t display;

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

    if (dwin_port_init(&display, &port_cfg) < 0)
    {
        ESP_LOGE(TAG, "Failed to bring the display up");
        return;
    }

    /* The display reboots after dwin_init(), give it time to come back. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint8_t gui_version = 0;
    uint8_t os_version = 0;
    if (dwin_get_version(&display, &gui_version, &os_version) == 0)
    {
        ESP_LOGI(TAG, "DGUS GUI v%d, OS v%d", gui_version, os_version);
    }

    dwin_set_brightness(&display, 80);
    dwin_change_page(&display, 1);
    dwin_write_text_field(&display, VP_TITLE, "Hello from ESP-IDF");
    dwin_set_color_text(&display, SP_TITLE, 0xF800); /* RGB565 red */

    struct tm now = {
        .tm_year = 126, /* 2026, tm_year counts from 1900 */
        .tm_mon = 8,
        .tm_mday = 11,
        .tm_wday = 5,
        .tm_hour = 10,
        .tm_min = 30,
        .tm_sec = 0,
    };
    dwin_update_rtc(&display, &now);

    uint16_t counter = 0;
    for (;;)
    {
        dwin_update_variable(&display, counter, VP_COUNTER);

        uint16_t read_back = 0;
        if (dwin_read_variable(&display, &read_back, VP_COUNTER) == 0)
        {
            ESP_LOGI(TAG, "VP 0x%04X = %d", VP_COUNTER, read_back);
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
