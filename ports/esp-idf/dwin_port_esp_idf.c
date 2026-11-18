/***********************************************************************************************************************
 *
 * @file    dwin_port_esp_idf.c
 * @brief   DWIN platform port for ESP-IDF.
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
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "esp_timer.h"

#include "dwin.h"
#include "dwin_port.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#define DWIN_PORT_TX_TIMEOUT_MS 1000

/****************************************************** Typedefs ******************************************************/

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_DECLARE();

/***************************************************** Variables ******************************************************/
static DWIN_Object_t *dwin_object;
static dwin_port_cfg_t dwin_port_cfg;
static SemaphoreHandle_t dwin_mutex;
static bool dwin_uart_installed;

/*********************************************** Internal Declarations ************************************************/
static uint32_t _dwin_io_get_tick_cb(void);
static void _dwin_io_receive_flush_buffer_cb(void);
static int16_t _dwin_io_receive_single_data_cb(uint8_t *pSingleData);
static int16_t _dwin_io_send_data_cb(uint8_t *pData, uint16_t Length);
static int8_t _dwin_io_deinit_cb(void);
static int8_t _dwin_io_init_cb(void);
static void _dwin_io_sleep_msec_cb(uint32_t ms);
static int8_t _dwin_io_mutex_lock_cb(uint32_t ms);
static void _dwin_io_mutex_unlock_cb(void);

/***************************************************** Functions ******************************************************/
int dwin_port_init(DWIN_Object_t *Obj, const dwin_port_cfg_t *cfg)
{
    int ret;

    if (Obj == NULL)
    {
        return -1;
    }

    if (cfg != NULL)
    {
        dwin_port_cfg = *cfg;
    }
    else
    {
        dwin_port_cfg_t defaults = DWIN_PORT_CFG_DEFAULT();
        dwin_port_cfg = defaults;
    }

    if (dwin_port_cfg.tx_pin < 0 || dwin_port_cfg.rx_pin < 0)
    {
        DWIN_LOG_ERR("TX and RX pins must be configured");
        return -1;
    }

    if (dwin_mutex == NULL)
    {
        dwin_mutex = xSemaphoreCreateMutex();
        if (dwin_mutex == NULL)
        {
            DWIN_LOG_ERR("Failed to create DWIN mutex");
            return -1;
        }
    }

    dwin_object = Obj;

    ret = dwin_register_bus_io(dwin_object,
                               _dwin_io_init_cb,
                               _dwin_io_deinit_cb,
                               _dwin_io_receive_flush_buffer_cb,
                               _dwin_io_send_data_cb,
                               _dwin_io_receive_single_data_cb,
                               _dwin_io_get_tick_cb,
                               _dwin_io_sleep_msec_cb,
                               _dwin_io_mutex_lock_cb,
                               _dwin_io_mutex_unlock_cb);
    if (ret < 0)
    {
        DWIN_LOG_ERR("Failed to register DWIN bus IO");
        return -1;
    }

    ret = dwin_init(dwin_object);
    if (ret < 0)
    {
        DWIN_LOG_ERR("Failed to initialize DWIN");
        return -1;
    }

    return 0;
}

int dwin_port_deinit(void)
{
    if (dwin_object == NULL)
    {
        return -1;
    }

    dwin_deinit(dwin_object);
    dwin_object = NULL;

    return 0;
}

DWIN_Object_t *dwin_port_get_object(void)
{
    return dwin_object;
}

/************************************************* Internal Functions *************************************************/

/************************************************* DWIN Bus IO CallBacks Functions *************************************************/
static int8_t _dwin_io_init_cb(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = dwin_port_cfg.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (dwin_uart_installed)
    {
        return 0;
    }

    if (uart_driver_install(dwin_port_cfg.uart_port,
                            dwin_port_cfg.rx_buffer_size,
                            dwin_port_cfg.tx_buffer_size,
                            0, NULL, 0) != ESP_OK)
    {
        DWIN_LOG_ERR("Failed to install the UART driver");
        return -1;
    }

    if (uart_param_config(dwin_port_cfg.uart_port, &uart_cfg) != ESP_OK)
    {
        DWIN_LOG_ERR("Failed to configure the UART parameters");
        uart_driver_delete(dwin_port_cfg.uart_port);
        return -1;
    }

    if (uart_set_pin(dwin_port_cfg.uart_port,
                     dwin_port_cfg.tx_pin,
                     dwin_port_cfg.rx_pin,
                     UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK)
    {
        DWIN_LOG_ERR("Failed to set the UART pins");
        uart_driver_delete(dwin_port_cfg.uart_port);
        return -1;
    }

    dwin_uart_installed = true;

    DWIN_LOG_INF("Display UART%d up at %d bauds (tx %d, rx %d)",
                 dwin_port_cfg.uart_port, dwin_port_cfg.baud_rate,
                 dwin_port_cfg.tx_pin, dwin_port_cfg.rx_pin);

    return 0;
}

static int8_t _dwin_io_deinit_cb(void)
{
    if (!dwin_uart_installed)
    {
        return 0;
    }

    uart_driver_delete(dwin_port_cfg.uart_port);
    dwin_uart_installed = false;

    return 0;
}

// callback to write out data (blocking until the bytes are queued)
static int16_t _dwin_io_send_data_cb(uint8_t *pData, uint16_t Length)
{
    int written = uart_write_bytes(dwin_port_cfg.uart_port, (const char *)pData, Length);
    if (written < 0)
    {
        return -1;
    }

    /* Make sure the frame has left the controller before we start polling for the
     * reply, the DGUS answers a few hundred microseconds after the last byte. */
    uart_wait_tx_done(dwin_port_cfg.uart_port, pdMS_TO_TICKS(DWIN_PORT_TX_TIMEOUT_MS));

    return (int16_t)written;
}

// callback to receive data from uart (non-blocking)
static int16_t _dwin_io_receive_single_data_cb(uint8_t *pSingleData)
{
    if (uart_read_bytes(dwin_port_cfg.uart_port, pSingleData, 1, 0) != 1)
    {
        return -1;
    }

    return 0;
}

// callback to flush receive buffer
static void _dwin_io_receive_flush_buffer_cb(void)
{
    uart_flush_input(dwin_port_cfg.uart_port);
}

// callback to get an estimative of elapsed time in ms
static uint32_t _dwin_io_get_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void _dwin_io_sleep_msec_cb(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);

    /* pdMS_TO_TICKS() rounds down, never let a sleep request become a busy loop. */
    vTaskDelay((ticks == 0) ? 1 : ticks);
}

static int8_t _dwin_io_mutex_lock_cb(uint32_t ms)
{
    return (xSemaphoreTake(dwin_mutex, pdMS_TO_TICKS(ms)) == pdTRUE) ? 0 : -1;
}

static void _dwin_io_mutex_unlock_cb(void)
{
    xSemaphoreGive(dwin_mutex);
}
