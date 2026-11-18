/***********************************************************************************************************************
 *
 * @file    dwin_port_zephyr.c
 * @brief   DWIN platform port for Zephyr.
 * @date    10/12/2025
 * @author  Fernando Fontes 
 * @note    Platform Target: Zephyr RTOS (validated on nRF54L15)
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

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include "dwin.h"
#include "dwin_port.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#ifndef CONFIG_DWIN_RING_BUFFER_SIZE
#define CONFIG_DWIN_RING_BUFFER_SIZE 50
#endif

#define RING_BUFFER_SIZE CONFIG_DWIN_RING_BUFFER_SIZE

/****************************************************** Typedefs ******************************************************/
typedef struct
{
    uint8_t data[RING_BUFFER_SIZE];
    uint16_t tail;
    uint16_t head;
} RingBuffer_t;

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_DECLARE();

#if DT_HAS_ALIAS(display_uart)
static const struct device *const default_uart_dev = DEVICE_DT_GET(DT_ALIAS(display_uart));
#else
static const struct device *const default_uart_dev = NULL;
#endif

/***************************************************** Variables ******************************************************/
static DWIN_Object_t *dwin_object;
static const struct device *display_uart_dev;
static RingBuffer_t UART_RxData = {0};
K_MUTEX_DEFINE(dwin_mutex);

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

static void serial_cb_int(const struct device *dev, void *user_data);

/***************************************************** Functions ******************************************************/
int dwin_port_init(DWIN_Object_t *Obj, const dwin_port_cfg_t *cfg)
{
    int ret;

    if (Obj == NULL)
    {
        return -1;
    }

    display_uart_dev = (cfg != NULL && cfg->uart_dev != NULL) ? cfg->uart_dev : default_uart_dev;
    if (display_uart_dev == NULL)
    {
        DWIN_LOG_ERR("No display UART: pass one in the config or add a display-uart alias");
        return -1;
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
    if (!device_is_ready(display_uart_dev))
    {
        DWIN_LOG_ERR("Display UART (alias display-uart) not ready");
        return -1;
    }

    /* configure interrupt and callback to receive data */
    int ret = uart_irq_callback_user_data_set(display_uart_dev, serial_cb_int, NULL);
    if (ret < 0)
    {
        if (ret == -ENOTSUP)
        {
            DWIN_LOG_ERR("Interrupt-driven UART API support not enabled");
        }
        else if (ret == -ENOSYS)
        {
            DWIN_LOG_ERR("UART device does not support interrupt-driven API");
        }
        else
        {
            DWIN_LOG_ERR("Error setting UART callback: %d", ret);
        }
        return -1;
    }

    uart_irq_rx_enable(display_uart_dev);

    return 0;
}

static int8_t _dwin_io_deinit_cb(void)
{
    uart_irq_rx_disable(display_uart_dev);
    return 0;
}

// callback to poll out data (blocking)
static int16_t _dwin_io_send_data_cb(uint8_t *pData, uint16_t Length)
{
    for (int i = 0; i < Length; i++)
    {
        uart_poll_out(display_uart_dev, pData[i]);
    }
    return Length;
}

// callback to receive data from uart (non-blocking)
static int16_t _dwin_io_receive_single_data_cb(uint8_t *pSingleData)
{
    /* Note: other possible implementation is to retrieve directly one data from UART buffer */
    /* without using the interrupt and the intermediate buffer */

    if (UART_RxData.head != UART_RxData.tail)
    {
        /* serial data available, so return data to user */
        *pSingleData = UART_RxData.data[UART_RxData.head++];
        /* check for ring buffer wrap */
        if (UART_RxData.head >= RING_BUFFER_SIZE)
        {
            /* ring buffer wrap, so reset head pointer to start of buffer */
            UART_RxData.head = 0;
        }
    }
    else
    {
        return -1;
    }

    return 0;
}

// callback to flush receive buffer
static void _dwin_io_receive_flush_buffer_cb(void)
{
    uart_irq_rx_disable(display_uart_dev);
    memset(UART_RxData.data, 0, RING_BUFFER_SIZE);
    UART_RxData.head = UART_RxData.tail = 0;
    uart_irq_rx_enable(display_uart_dev);
}

// callback to get an estimative of elapsed time in ms
static uint32_t _dwin_io_get_tick_cb(void)
{
    return (uint32_t)k_uptime_get();
}

static void _dwin_io_sleep_msec_cb(uint32_t ms)
{
    k_msleep(ms);
}

static int8_t _dwin_io_mutex_lock_cb(uint32_t ms)
{
    return (k_mutex_lock(&dwin_mutex, K_MSEC(ms)) == 0) ? 0 : -1;
}

static void _dwin_io_mutex_unlock_cb(void)
{
    k_mutex_unlock(&dwin_mutex);
}

/*********************************************** Callback Functions ****************************************/
static void serial_cb_int(const struct device *dev, void *user_data)
{
    uint8_t c;

    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    /* The return value is not used: it is void from Zephyr 4.5 on, and before that every driver
     * returned 1. */
    (void)uart_irq_update(display_uart_dev);

    if (!uart_irq_rx_ready(display_uart_dev))
    {
        return;
    }

    /* read until FIFO empty */
    while (uart_fifo_read(display_uart_dev, &c, 1) == 1)
    {
        UART_RxData.data[UART_RxData.tail] = c;

        /* If ring buffer end is reached reset tail pointer to start of buffer */
        if (++UART_RxData.tail >= RING_BUFFER_SIZE)
        {
            UART_RxData.tail = 0;
        }
    }
}
