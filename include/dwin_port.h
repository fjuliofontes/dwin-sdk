/***********************************************************************************************************************
 *
 * @file    dwin_port.h
 * @brief   DWIN platform port (bus IO glue) header file.
 * @date    10/12/2025
 * @author  Fernando Fontes 
 * @note    Platform Target: portable (ESP-IDF / Zephyr)
 *
 * @details Ready made bus IO implementation for the supported platforms. It is
 *          entirely optional: an application can implement the nine callbacks of
 *          DWIN_IO_t itself and call dwin_register_bus_io() directly.
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_PORT_H_
#define DWIN_PORT_H_

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdint.h>

#include "dwin.h"

#if defined(ESP_PLATFORM)
#include "driver/uart.h"
#elif defined(__ZEPHYR__)
#include <zephyr/device.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************** Defines *******************************************************/

/****************************************************** Typedefs ******************************************************/
#if defined(ESP_PLATFORM)

typedef struct
{
    /** UART controller to use, e.g. UART_NUM_1. */
    uart_port_t uart_port;
    /** TX pin, connected to the display RX pin. */
    int tx_pin;
    /** RX pin, connected to the display TX pin. */
    int rx_pin;
    /** Baud rate. DGUS displays ship at 115200 by default. */
    int baud_rate;
    /** Driver RX ring buffer size, in bytes. Must be larger than 128. */
    int rx_buffer_size;
    /** Driver TX ring buffer size, in bytes. 0 selects a blocking, unbuffered TX. */
    int tx_buffer_size;
} dwin_port_cfg_t;

#define DWIN_PORT_CFG_DEFAULT()        \
    {                                  \
        .uart_port = UART_NUM_1,       \
        .tx_pin = -1,                  \
        .rx_pin = -1,                  \
        .baud_rate = 115200,           \
        .rx_buffer_size = 1024,        \
        .tx_buffer_size = 0,           \
    }

#elif defined(__ZEPHYR__)

typedef struct
{
    /** UART device to use. NULL selects DT_ALIAS(display_uart). */
    const struct device *uart_dev;
} dwin_port_cfg_t;

#define DWIN_PORT_CFG_DEFAULT() \
    {                           \
        .uart_dev = NULL,       \
    }

#endif

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/
/**
 *
 * @brief   Register the platform bus IO on the object and initialize the display.
 * @param   Obj   Pointer to the DWIN object to bring up.
 * @param   cfg   Platform specific bus configuration, or NULL for the defaults.
 * @note    Only one DWIN object can be served by this port at a time.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_port_init(DWIN_Object_t *Obj, const dwin_port_cfg_t *cfg);

/**
 *
 * @brief   Release the bus owned by the port.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_port_deinit(void);

/**
 *
 * @brief   Get the DWIN object registered by dwin_port_init().
 * @return  Returns the DWIN object, or NULL when the port is not initialized.
 *
 **/
DWIN_Object_t *dwin_port_get_object(void);

#ifdef __cplusplus
}
#endif

#endif
