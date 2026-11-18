/***********************************************************************************************************************
 *
 * @file    main.c
 * @brief   DWIN SDK basic example for Zephyr: page, text, variable and brightness.
 * @date    18/11/2026
 * @author  Fernando Fontes
 * @note    Platform Target: Zephyr RTOS
 *
 * SPDX-License-Identifier: MIT
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dwin.h"
#include "dwin_port.h"

/****************************************************** Defines *******************************************************/
/* VP addresses of the DGUS project. Replace with the ones from your own project. */
#define VP_COUNTER 0x1000
#define VP_TITLE 0x2000
#define SP_TITLE 0x5000

/***************************************************** Variables ******************************************************/
LOG_MODULE_REGISTER(example, LOG_LEVEL_INF);

static DWIN_Object_t display;

/***************************************************** Functions ******************************************************/
int main(void)
{
    /* NULL takes the UART of the display-uart devicetree alias. */
    if (dwin_port_init(&display, NULL) < 0)
    {
        LOG_ERR("Failed to bring the display up");
        return 0;
    }

    /* The display reboots after dwin_init(), give it time to come back. */
    k_msleep(3000);

    uint8_t gui_version = 0;
    uint8_t os_version = 0;
    if (dwin_get_version(&display, &gui_version, &os_version) == 0)
    {
        LOG_INF("DGUS GUI v%d, OS v%d", gui_version, os_version);
    }

    dwin_set_brightness(&display, 80);
    dwin_change_page(&display, 1);
    dwin_write_text_field(&display, VP_TITLE, "Hello from Zephyr");
    dwin_set_color_text(&display, SP_TITLE, 0xF800); /* RGB565 red */

    uint16_t counter = 0;
    for (;;)
    {
        dwin_update_variable(&display, counter, VP_COUNTER);

        uint16_t read_back = 0;
        if (dwin_read_variable(&display, &read_back, VP_COUNTER) == 0)
        {
            LOG_INF("VP 0x%04X = %d", VP_COUNTER, read_back);
        }

        counter++;
        k_msleep(1000);
    }

    return 0;
}
