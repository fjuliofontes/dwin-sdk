/***********************************************************************************************************************
 *
 * @file    dwin.c
 * @brief   DWIN DGUS display driver implementation.
 * @date    07/11/2025
 * @author  Fernando Fontes 
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
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

#include "dwin.h"
#include "dwin_log.h"

/****************************************************** Defines *******************************************************/
#define HI8(x) ((((x) >> 8) & 0xFF))
#define LO8(x) (((x) & 0xFF))

#define DWIN_START_OF_FRAME {0x5A, 0xA5}
#define DWIN_CMD_RESET {0x55, 0xaa, 0x5a, 0xa5}
#define DWIN_CMD_PAUSE {0x55, 0xaa, 0x5a, 0xa5}
#define DWIN_CMD_ACK {0x4F, 0x4B}

#define DWIN_ENABLE_EXTERNAL_MEMORY_OPERATION 0x5A
#define DWIN_WRITE_32KBYTES_DATA_BLOCK 0x02
#define DWIN_ENABLE_PAGE_OPERATION 0x5A
#define DWIN_PAGE_SWITCH 0x01

/* Frame overhead accounted for by the DGUS length byte: VP command + VP address. */
#define DWIN_FRAME_OVERHEAD 3

/* Longest text a single write frame can carry: the payload also holds the 0xFFFF end of text. */
#define DWIN_TEXT_MAX_BYTES (DWIN_MAX_PAYLOAD_SIZE - 2)

/* Fixed offsets inside a reply frame. */
#define DWIN_RESP_OFFSET_LENGTH 2
#define DWIN_RESP_OFFSET_CMD 3
#define DWIN_RESP_OFFSET_ADDRESS 4
#define DWIN_RESP_OFFSET_WORD_COUNT 6
#define DWIN_RESP_OFFSET_PAYLOAD 7

/****************************************************** Typedefs ******************************************************/
typedef enum
{
    DWIN_VP_ADDRESS_PAUSE_DISPLAY = 0x00fc,
    DWIN_VP_ADDRESS_RESET_DISPLAY = 0x0004,
    DWIN_VP_WRITE_NOR_FLASH_OPERATION = 0x00aa,
    DWIN_VP_APPLICATION_VERSION = 0x000F,
    DWIN_VP_PIC_SET_PAGE = 0x0084,
    DWIN_VP_UPDATE_RTC = 0x0010,
    DWIN_VP_ADDRESS_BRIGHTNESS = 0x0082,
} dwin_vp_address_t;

typedef enum
{
    DWIN_VP_CMD_WRITE = 0x82,
    DWIN_VP_CMD_READ = 0x83,
} dwin_vp_cmd_t;

/***************************************************** Constants ******************************************************/
DWIN_LOG_MODULE_REGISTER();

/***************************************************** Variables ******************************************************/
static uint8_t dwin_start_of_frame[] = DWIN_START_OF_FRAME;

/*********************************************** Internal Declarations ************************************************/
static int32_t time_left_from_expiration(uint32_t init, uint32_t now, uint32_t timeout);
static int dwin_receive_response(DWIN_Object_t *Obj, uint8_t *pData, uint16_t *pDataSize, uint32_t timeout);
static int dwin_wait_for_ack(DWIN_Object_t *Obj);
static void dwin_send_frame(DWIN_Object_t *Obj, uint8_t vp_cmd, uint16_t address, const uint8_t *pPayload, uint8_t payload_len);
static int dwin_transact_write(DWIN_Object_t *Obj, uint16_t address, const uint8_t *pPayload, uint8_t payload_len, bool wait_ack);
static bool dwin_is_ready(DWIN_Object_t *Obj);
static uint8_t utf8_to_latin1(const char *in, uint8_t *out, uint8_t out_size);

/***************************************************** Functions ******************************************************/
int dwin_register_bus_io(DWIN_Object_t *Obj, IO_Init_Func IO_Init, IO_DeInit_Func IO_DeInit, IO_Flush_Func IO_FlushBuffer,
                         IO_Send_Func IO_Send, IO_ReceiveOne_Func IO_ReceiveOne, IO_GetTickCb_Func IO_GetTickCb, IO_SleepMsec_Func IO_SleepMsec,
                         IO_MutexLock_Func IO_MutexLock, IO_MutexUnlock_Func IO_MutexUnlock)
{
    if (Obj == NULL)
    {
        return -1;
    }

    Obj->fops.IO_Init = IO_Init;
    Obj->fops.IO_DeInit = IO_DeInit;
    Obj->fops.IO_FlushBuffer = IO_FlushBuffer;
    Obj->fops.IO_Send = IO_Send;
    Obj->fops.IO_ReceiveOne = IO_ReceiveOne;
    Obj->fops.IO_GetTickCb = IO_GetTickCb;
    Obj->fops.IO_SleepMsec = IO_SleepMsec;
    Obj->fops.IO_MutexLock = IO_MutexLock;
    Obj->fops.IO_MutexUnlock = IO_MutexUnlock;

    Obj->LastVarStorageWriteSize = 0;
    Obj->LastNorFlashWriteTick = 0;
    Obj->NorFlashWritten = false;

    return 0;
}

int dwin_init(DWIN_Object_t *Obj)
{
    if (Obj == NULL || Obj->fops.IO_Init == NULL)
    {
        DWIN_LOG_ERR("IO_Init function is not set");
        return -1;
    }

    if (Obj->fops.IO_Init() < 0)
    {
        DWIN_LOG_ERR("Failed to initialize DWIN bus IO");
        return -1;
    }

    // soft reset the display to ensure it is in a known state
    dwin_reset_display(Obj);

    return 0;
}

int dwin_deinit(DWIN_Object_t *Obj)
{
    if (Obj == NULL || Obj->fops.IO_DeInit == NULL)
    {
        return -1;
    }

    return (Obj->fops.IO_DeInit() < 0) ? -1 : 0;
}

int dwin_get_version(DWIN_Object_t *Obj, uint8_t *gui_version, uint8_t *os_version)
{
    if (!dwin_is_ready(Obj) || gui_version == NULL || os_version == NULL)
    {
        return -1;
    }

    uint16_t dummy = 0x00;
    int ret = dwin_read_variable(Obj, &dummy, DWIN_VP_APPLICATION_VERSION);
    if (ret < 0)
    {
        return -1;
    }

    *gui_version = (uint8_t)HI8(dummy);
    *os_version = (uint8_t)LO8(dummy);

    return 0;
}

int dwin_change_page(DWIN_Object_t *Obj, uint8_t page)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    uint8_t cmd[] = {DWIN_ENABLE_PAGE_OPERATION, DWIN_PAGE_SWITCH, 0x00, page};

    return dwin_transact_write(Obj, DWIN_VP_PIC_SET_PAGE, cmd, sizeof(cmd), true);
}

int dwin_update_variable(DWIN_Object_t *Obj, uint16_t value, uint16_t address)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    // Frame: 5A A5 05 82 <addrH> <addrL> <ValH> <ValL>
    uint8_t cmd[] = {HI8(value), LO8(value)};

    return dwin_transact_write(Obj, address, cmd, sizeof(cmd), true);
}

int dwin_set_color_text(DWIN_Object_t *Obj, uint16_t sp_address, uint16_t color)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    /* The colour attribute lives 3 words after the descriptor base address. */
    uint16_t sp_address_color = sp_address + 3;
    uint8_t cmd[] = {HI8(color), LO8(color)};

    return dwin_transact_write(Obj, sp_address_color, cmd, sizeof(cmd), true);
}

int dwin_change_icon_image(DWIN_Object_t *Obj, uint16_t vp_address, uint16_t image_id)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    uint8_t cmd[] = {HI8(image_id), LO8(image_id)};

    return dwin_transact_write(Obj, vp_address, cmd, sizeof(cmd), true);
}

int dwin_write_variables(DWIN_Object_t *Obj, uint16_t address, const uint8_t *pData, uint8_t len)
{
    if (!dwin_is_ready(Obj) || pData == NULL || len == 0 || len > DWIN_MAX_PAYLOAD_SIZE)
    {
        return -1;
    }

    return dwin_transact_write(Obj, address, pData, len, true);
}

int dwin_read_variable(DWIN_Object_t *Obj, uint16_t *value, uint16_t address)
{
    return dwin_read_variables(Obj, value, address, 1);
}

int dwin_read_variables(DWIN_Object_t *Obj, uint16_t *values, uint16_t address, uint8_t word_count)
{
    if (!dwin_is_ready(Obj) || Obj->fops.IO_FlushBuffer == NULL || values == NULL || word_count == 0)
    {
        return -1;
    }

    uint16_t cmd_reply_size = DWIN_RESP_OFFSET_PAYLOAD + (word_count * DWIN_VARIABLE_STORAGE_WORD_SIZE);
    if (cmd_reply_size > DWIN_CMD_SIZE)
    {
        DWIN_LOG_ERR("Reply of %d bytes does not fit in DWIN_CMD_SIZE (%d)", cmd_reply_size, DWIN_CMD_SIZE);
        return -1;
    }

    // Lock the mutex
    if (Obj->fops.IO_MutexLock(DWIN_MUTEX_TIMEOUT_MS) < 0)
    {
        DWIN_LOG_ERR("Failed to lock DWIN mutex");
        return -1;
    }

    int ret = 0;

    Obj->fops.IO_FlushBuffer();

    uint8_t vp_address[] = {HI8(address), LO8(address)};

    dwin_send_frame(Obj, DWIN_VP_CMD_READ, address, &word_count, sizeof(word_count));

    DWIN_LOG_DBG("Waiting for reply...");
    Obj->CmdRespSize = sizeof(Obj->CmdResp); // Maximum buffer size
    ret = dwin_receive_response(Obj, Obj->CmdResp, &Obj->CmdRespSize, DWIN_RESPONSE_TIMEOUT_MS);
    if (ret < 0)
    {
        DWIN_LOG_ERR("Failed to receive response");
        ret = -1;
        goto unlock_and_return;
    }

    if (Obj->CmdRespSize != cmd_reply_size)
    {
        DWIN_LOG_ERR("Response size does not match expected size: %d != %d", Obj->CmdRespSize, cmd_reply_size);
        ret = -1;
        goto unlock_and_return;
    }

    if (Obj->CmdResp[DWIN_RESP_OFFSET_CMD] != DWIN_VP_CMD_READ)
    {
        DWIN_LOG_ERR("Response does not match expected VP command");
        ret = -1;
        goto unlock_and_return;
    }

    if (memcmp(&Obj->CmdResp[DWIN_RESP_OFFSET_ADDRESS], vp_address, sizeof(vp_address)) != 0)
    {
        DWIN_LOG_ERR("Response does not match expected VP address");
        ret = -1;
        goto unlock_and_return;
    }

    if (Obj->CmdResp[DWIN_RESP_OFFSET_WORD_COUNT] != word_count)
    {
        DWIN_LOG_ERR("Response does not match expected number of words");
        ret = -1;
        goto unlock_and_return;
    }

    for (uint8_t i = 0; i < word_count; i++)
    {
        uint16_t offset = DWIN_RESP_OFFSET_PAYLOAD + (i * DWIN_VARIABLE_STORAGE_WORD_SIZE);
        values[i] = (Obj->CmdResp[offset] << 8) | Obj->CmdResp[offset + 1];
    }

unlock_and_return:
    // Unlock the mutex
    Obj->fops.IO_MutexUnlock();

    return ret;
}

int dwin_update_rtc(DWIN_Object_t *Obj, struct tm *tm_buffer)
{
    if (!dwin_is_ready(Obj) || tm_buffer == NULL)
    {
        return -1;
    }

    /*Frame: 5A A5 0B 82 00 10 13 0A 01 00 0B 0C 0D 00
     D7=Year (0-0x63) D6=month(0-0x0C), D5=day(0-0x1F), D4=week(0-0x6),D3=hour(0-0x17), D2=minute(0-0x3B), D1=second(0-0x3B), D0undefined. */
    uint8_t cmd[] = {
        (tm_buffer->tm_year - 100), tm_buffer->tm_mon,
        tm_buffer->tm_mday, tm_buffer->tm_wday,
        tm_buffer->tm_hour, tm_buffer->tm_min,
        tm_buffer->tm_sec, 0x00};

    return dwin_transact_write(Obj, DWIN_VP_UPDATE_RTC, cmd, sizeof(cmd), true);
}

int dwin_pause_display(DWIN_Object_t *Obj)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    uint8_t cmd[] = DWIN_CMD_PAUSE;

    return dwin_transact_write(Obj, DWIN_VP_ADDRESS_PAUSE_DISPLAY, cmd, sizeof(cmd), true);
}

int dwin_reset_display(DWIN_Object_t *Obj)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    uint8_t cmd[] = DWIN_CMD_RESET;

    // no ack is returned when resetting the display
    return dwin_transact_write(Obj, DWIN_VP_ADDRESS_RESET_DISPLAY, cmd, sizeof(cmd), false);
}

int dwin_set_brightness(DWIN_Object_t *Obj, uint8_t brightness)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    if (brightness > 100)
    {
        return -1;
    }

    return dwin_transact_write(Obj, DWIN_VP_ADDRESS_BRIGHTNESS, &brightness, sizeof(brightness), true);
}

int dwin_update_selector(DWIN_Object_t *Obj, uint16_t address, uint16_t x, uint16_t y)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    uint8_t cmd[] = {
        HI8(x),
        LO8(x),
        HI8(y),
        LO8(y),
    };

    return dwin_transact_write(Obj, address, cmd, sizeof(cmd), true);
}

int dwin_write_text_field(DWIN_Object_t *Obj, uint16_t address, const char *text)
{
    if (!dwin_is_ready(Obj) || Obj->fops.IO_FlushBuffer == NULL || text == NULL)
    {
        return -1;
    }

    // the text fields use the panel's 8-bit encoding, where font 0 holds Latin-1 (accented letters)
    // one spare byte so that a text longer than a frame can be told apart from one that just fits
    uint8_t latin1[DWIN_TEXT_MAX_BYTES + 1];
    uint8_t text_len = utf8_to_latin1(text, latin1, sizeof(latin1));
    uint8_t end_of_text[] = {0xFF, 0xFF};

    if (text_len > DWIN_TEXT_MAX_BYTES)
    {
        DWIN_LOG_ERR("Text exceeds the maximum of %u characters", (unsigned int)DWIN_TEXT_MAX_BYTES);
        return -1;
    }

    // Lock the mutex
    if (Obj->fops.IO_MutexLock(DWIN_MUTEX_TIMEOUT_MS) < 0)
    {
        DWIN_LOG_ERR("Failed to lock DWIN mutex");
        return -1;
    }

    int ret = 0;

    Obj->fops.IO_FlushBuffer();

    uint8_t vp_cmd = DWIN_VP_CMD_WRITE;
    uint8_t vp_address[] = {HI8(address), LO8(address)};
    uint8_t cmd_len = text_len + DWIN_FRAME_OVERHEAD + sizeof(end_of_text);

    Obj->fops.IO_Send(dwin_start_of_frame, sizeof(dwin_start_of_frame));
    Obj->fops.IO_Send(&cmd_len, sizeof(cmd_len));
    Obj->fops.IO_Send(&vp_cmd, sizeof(vp_cmd));
    Obj->fops.IO_Send(vp_address, sizeof(vp_address));
    Obj->fops.IO_Send(latin1, text_len);
    Obj->fops.IO_Send(end_of_text, sizeof(end_of_text));

    DWIN_LOG_DBG("Waiting for ACK...");
    ret = dwin_wait_for_ack(Obj);
    if (ret < 0)
    {
        DWIN_LOG_ERR("Failed to receive ACK");
        ret = -1;
        goto unlock_and_return;
    }
    DWIN_LOG_DBG("ACK received");

unlock_and_return:
    // Unlock the mutex
    Obj->fops.IO_MutexUnlock();

    return ret;
}

int dwin_read_text_field(DWIN_Object_t *Obj, uint16_t address, char *text, uint8_t text_size)
{
    if (!dwin_is_ready(Obj) || text == NULL || text_size == 0)
    {
        return -1;
    }

    int8_t ret = 0;
    uint16_t value = 0;
    uint8_t read_count = 0;
    do
    {
        ret = dwin_read_variable(Obj, &value, address + (read_count / 2));
        if (ret < 0)
        {
            return -1;
        }

        // is it string end?
        if (value != 0xFFFF && value != 0x0000)
        {
            text[read_count++] = HI8(value);
            text[read_count++] = LO8(value);
        }
        else
        {
            break;
        }

    } while (read_count < (text_size - 1));

    // add the null terminator
    text[read_count] = '\0';

    return 0;
}

int dwin_toggle_ui_element(DWIN_Object_t *Obj, uint16_t sp, uint16_t vp, bool show)
{
    if (!dwin_is_ready(Obj))
    {
        return -1;
    }

    uint8_t cmd[] = {
        show ? HI8(vp) : 0xFF,
        show ? LO8(vp) : 0x00};

    return dwin_transact_write(Obj, sp, cmd, sizeof(cmd), true);
}

int dwin_write_data_to_variable_storage(DWIN_Object_t *Obj, uint8_t *pData, uint16_t data_size, uint16_t fragment)
{
    if (!dwin_is_ready(Obj) || Obj->fops.IO_FlushBuffer == NULL || pData == NULL || data_size == 0)
    {
        return -1;
    }

    const uint16_t fragment_bytes = DWIN_VARIABLE_STORAGE_FRAGMENT_SIZE * DWIN_VARIABLE_STORAGE_WORD_SIZE;

    // Check if data is fragmentated correctly
    if (fragment > 0 && (Obj->LastVarStorageWriteSize != fragment_bytes))
    {
        DWIN_LOG_ERR("Data is not fragmentated correctly. Aborting!");
        DWIN_LOG_INF("Call again this function with fragment=0 to reset!");
        return -1;
    }

    // Check if the data size is greater than the fragment size
    if (data_size > fragment_bytes)
    {
        DWIN_LOG_ERR("Fragment size is: %d bytes. The write data must be divided in multiple fragments.", fragment_bytes);
        return -1;
    }

    uint16_t vp_address = DWIN_VARIABLE_STORAGE_START_UPDATE_SPACE_ADDRESS + (fragment * DWIN_VARIABLE_STORAGE_FRAGMENT_SIZE);

    if (vp_address > (DWIN_VARIABLE_STORAGE_END_UPDATE_SPACE_ADDRESS - DWIN_VARIABLE_STORAGE_FRAGMENT_SIZE))
    {
        DWIN_LOG_ERR("Fragment is out of range. Aborting!");
        return -1;
    }

    int ret = -1;

    // the same bytes at the same address: sending a lost frame again does no harm
    for (uint8_t attempt = 0; (attempt <= DWIN_OTA_WRITE_RETRIES) && (ret < 0); attempt++)
    {
        if (attempt > 0)
        {
            DWIN_LOG_WRN("Fragment %d not acknowledged, sending it again (%d/%d)", fragment, attempt, DWIN_OTA_WRITE_RETRIES);
        }

        ret = dwin_transact_write(Obj, vp_address, pData, (uint8_t)data_size, true);
    }

    if (ret == 0)
    {
        Obj->LastVarStorageWriteSize = data_size;
    }

    return ret;
}

int dwin_write_from_variable_storage_to_nor_flash(DWIN_Object_t *Obj, uint16_t destination_id)
{
    if (!dwin_is_ready(Obj) || Obj->fops.IO_FlushBuffer == NULL || Obj->fops.IO_GetTickCb == NULL ||
        Obj->fops.IO_SleepMsec == NULL)
    {
        return -1;
    }

#if DWIN_NOR_FLASH_WAIT_READY
    const uint16_t delay_field = DWIN_NOR_FLASH_READY_DELAY_FIELD;
#else
    const uint16_t delay_field = DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS;

    // Wait for the NOR flash to be ready
    if (Obj->NorFlashWritten)
    {
        uint32_t elapsed = Obj->fops.IO_GetTickCb() - Obj->LastNorFlashWriteTick;
        if (elapsed < DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS)
        {
            DWIN_LOG_INF("Waiting for NOR flash to be ready.");
            Obj->fops.IO_SleepMsec(DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS - elapsed);
        }
    }
#endif

    uint8_t cmd[] = {DWIN_ENABLE_EXTERNAL_MEMORY_OPERATION,
                     DWIN_WRITE_32KBYTES_DATA_BLOCK,
                     HI8(destination_id), LO8(destination_id),
                     HI8(DWIN_VARIABLE_STORAGE_START_UPDATE_SPACE_ADDRESS), LO8(DWIN_VARIABLE_STORAGE_START_UPDATE_SPACE_ADDRESS),
                     HI8(delay_field), LO8(delay_field),
                     0x00, 0x00, 0x00, 0x00};

    // not sent again when the ACK is missing: the display may be committing the block already
    int ret = dwin_transact_write(Obj, DWIN_VP_WRITE_NOR_FLASH_OPERATION, cmd, sizeof(cmd), true);
    if (ret < 0)
    {
        return -1;
    }

    // Update the last write on NOR flash operation time
    Obj->LastNorFlashWriteTick = Obj->fops.IO_GetTickCb();
    Obj->NorFlashWritten = true;

    /* A fresh fragment sequence starts after every flash commit. */
    Obj->LastVarStorageWriteSize = 0;

    uint16_t external_memory_operation_status = 0;

#if DWIN_NOR_FLASH_WAIT_READY
    // The display clears the enable byte once the block is in the flash. It may not answer
    // while it is busy, so a failed read only means "not yet".
    for (;;)
    {
        ret = dwin_read_variable(Obj, &external_memory_operation_status, DWIN_VP_WRITE_NOR_FLASH_OPERATION);
        if (ret == 0 && HI8(external_memory_operation_status) != DWIN_ENABLE_EXTERNAL_MEMORY_OPERATION)
        {
            DWIN_LOG_DBG("Block committed in %u ms", (unsigned int)(Obj->fops.IO_GetTickCb() - Obj->LastNorFlashWriteTick));
            return 0;
        }

        if ((Obj->fops.IO_GetTickCb() - Obj->LastNorFlashWriteTick) >= DWIN_NOR_FLASH_READY_TIMEOUT_MS)
        {
            DWIN_LOG_ERR("Display did not finish the flash write in %d ms. Status: %04X", DWIN_NOR_FLASH_READY_TIMEOUT_MS,
                         external_memory_operation_status);
            return -1;
        }

        Obj->fops.IO_SleepMsec(DWIN_NOR_FLASH_READY_POLL_MS);
    }
#else
    // Check if the external memory operation was successful
    ret = dwin_read_variable(Obj, &external_memory_operation_status, DWIN_VP_WRITE_NOR_FLASH_OPERATION);
    if (ret < 0 || (external_memory_operation_status & (DWIN_ENABLE_EXTERNAL_MEMORY_OPERATION << 8)) != 0)
    {
        DWIN_LOG_ERR("External memory operation failed. Status: %04X", external_memory_operation_status);
        return -1;
    }

    return 0;
#endif
}

/************************************************* Internal Functions *************************************************/
/**
 * @brief   Check that the object and the mandatory bus IO callbacks are usable.
 * @param   In:   Obj   Pointer to the DWIN object.
 * @retval  true when the object can be used to talk to the display.
 */
static bool dwin_is_ready(DWIN_Object_t *Obj)
{
    return (Obj != NULL) &&
           (Obj->fops.IO_Send != NULL) &&
           (Obj->fops.IO_FlushBuffer != NULL) &&
           (Obj->fops.IO_MutexLock != NULL) &&
           (Obj->fops.IO_MutexUnlock != NULL);
}

/**
 * @brief   Emit a complete DGUS frame: 5A A5 <len> <cmd> <addrH> <addrL> <payload...>.
 * @param   In:   Obj           Pointer to the DWIN object.
 * @param   In:   vp_cmd        DGUS command byte (0x82 write / 0x83 read).
 * @param   In:   address       VP address.
 * @param   In:   pPayload      Payload bytes.
 * @param   In:   payload_len   Payload length.
 */
static void dwin_send_frame(DWIN_Object_t *Obj, uint8_t vp_cmd, uint16_t address, const uint8_t *pPayload, uint8_t payload_len)
{
    uint8_t vp_address[] = {HI8(address), LO8(address)};
    uint8_t cmd_len = payload_len + DWIN_FRAME_OVERHEAD;

    Obj->fops.IO_Send(dwin_start_of_frame, sizeof(dwin_start_of_frame));
    Obj->fops.IO_Send(&cmd_len, sizeof(cmd_len));
    Obj->fops.IO_Send(&vp_cmd, sizeof(vp_cmd));
    Obj->fops.IO_Send(vp_address, sizeof(vp_address));
    Obj->fops.IO_Send((uint8_t *)pPayload, payload_len);
}

/**
 * @brief   Run a complete write transaction: lock, flush, send and wait for the ACK.
 * @param   In:   Obj           Pointer to the DWIN object.
 * @param   In:   address       VP address.
 * @param   In:   pPayload      Payload bytes.
 * @param   In:   payload_len   Payload length.
 * @param   In:   wait_ack      Whether the display replies with an ACK to this command.
 * @retval  Returns 0 on success, otherwise an error code.
 */
static int dwin_transact_write(DWIN_Object_t *Obj, uint16_t address, const uint8_t *pPayload, uint8_t payload_len, bool wait_ack)
{
    // Lock the mutex
    if (Obj->fops.IO_MutexLock(DWIN_MUTEX_TIMEOUT_MS) < 0)
    {
        DWIN_LOG_ERR("Failed to lock DWIN mutex");
        return -1;
    }

    int ret = 0;

    Obj->fops.IO_FlushBuffer();

    dwin_send_frame(Obj, DWIN_VP_CMD_WRITE, address, pPayload, payload_len);

    if (!wait_ack)
    {
        goto unlock_and_return;
    }

    DWIN_LOG_DBG("Waiting for ACK...");
    ret = dwin_wait_for_ack(Obj);
    if (ret < 0)
    {
        DWIN_LOG_ERR("Failed to receive ACK");
        ret = -1;
        goto unlock_and_return;
    }
    DWIN_LOG_DBG("ACK received");

unlock_and_return:
    // Unlock the mutex
    Obj->fops.IO_MutexUnlock();

    return ret;
}

/**
 * @brief   Return the integer difference between 'init + timeout' and 'now'.
 *          The implementation is robust to uint32_t overflows.
 * @param   In:   init      Reference index.
 * @param   In:   now       Current index.
 * @param   In:   timeout   Target index.
 * @retval  Number of ms from now to target (init + timeout).
 */
static int32_t time_left_from_expiration(uint32_t init, uint32_t now, uint32_t timeout)
{
    int32_t ret = 0;
    uint32_t elapsed = 0;

    if (now >= init)
    {
        elapsed = now - init;
    }
    else
    { /* Timer wrap-around detected */
        elapsed = (UINT32_MAX - init) + now + 1;
    }

    if (elapsed >= timeout)
    {
        ret = 0; // Timeout expired
    }
    else
    {
        ret = (int32_t)(timeout - elapsed);
    }

    return ret;
}

static int dwin_receive_response(DWIN_Object_t *Obj, uint8_t *pData, uint16_t *pDataSize, uint32_t timeout)
{
    if (Obj == NULL || Obj->fops.IO_ReceiveOne == NULL || Obj->fops.IO_GetTickCb == NULL || pData == NULL || pDataSize == NULL)
    {
        return -1;
    }

    uint32_t tickstart = Obj->fops.IO_GetTickCb();
    uint16_t ReceivedData = 0;
    uint16_t ReadData = 0;
    uint16_t CommandLength = UINT16_MAX;
    uint8_t c;

    while ((time_left_from_expiration(tickstart, Obj->fops.IO_GetTickCb(), timeout) > 0) && (ReadData < CommandLength))
    {
        if (Obj->fops.IO_ReceiveOne(&c) != 0) /* Receive one sample from UART */
        {
            /* No data available yet, yield the CPU and poll again. Sleeping only
             * when the bus is idle keeps back-to-back bytes running at full speed. */
            Obj->fops.IO_SleepMsec(1);
            continue;
        }

        ReceivedData++;

        switch (ReadData)
        {
        case 0:
            if (c != dwin_start_of_frame[0])
            {
                ReadData = 0; // Reset on invalid start byte
                continue;
            }
            break;
        case 1:
            if (c != dwin_start_of_frame[1])
            {
                ReadData = 0; // Reset on invalid second byte
                continue;
            }
            break;
        case DWIN_RESP_OFFSET_LENGTH:
            CommandLength = c + ReadData + 1;
            break;
        }

        /* serial data available, so return data to user */
        if (ReadData < *pDataSize)
        {
            pData[ReadData++] = c;
        }
        else
        {
            // buffer overflow
            DWIN_LOG_ERR("Buffer overflow: %d", ReadData);
            break;
        }
    }

    if (ReceivedData == 0)
    {
        DWIN_LOG_ERR("Timeout: No data received");
        return -1;
    }

    else if (ReadData == 0)
    {
        DWIN_LOG_ERR("Timeout: Start of frame not received");
        return -1;
    }

    else if (ReadData != CommandLength)
    {
        DWIN_LOG_ERR("Timeout: Command length mismatch: %d != %d", ReadData, CommandLength);
        return -1;
    }

    *pDataSize = ReadData;

    return 0;
}

static int dwin_wait_for_ack(DWIN_Object_t *Obj)
{
    static uint8_t cmd_ack[] = DWIN_CMD_ACK;
    static const uint8_t cmd_reply_size = sizeof(dwin_start_of_frame) + sizeof(cmd_ack) + 1 + 1; // len + vp

    if (Obj == NULL)
    {
        return -1;
    }

    int ret;
    // Wait up to DWIN_RESPONSE_TIMEOUT_MS
    Obj->CmdRespSize = sizeof(Obj->CmdResp); // Maximum buffer size
    ret = dwin_receive_response(Obj, Obj->CmdResp, &Obj->CmdRespSize, DWIN_RESPONSE_TIMEOUT_MS);
    if (ret < 0)
    {
        DWIN_LOG_ERR("Failed to receive response");
        return -1;
    }
    DWIN_LOG_HEXDUMP_DBG(Obj->CmdResp, Obj->CmdRespSize, "Response:");

    if (Obj->CmdRespSize != cmd_reply_size)
    {
        DWIN_LOG_ERR("Response size does not match expected size: %d != %d", Obj->CmdRespSize, cmd_reply_size);
        return -1;
    }

    /* Check if the response starts with the start of frame */
    if (memcmp(&Obj->CmdResp[0], dwin_start_of_frame, sizeof(dwin_start_of_frame)) != 0)
    {
        DWIN_LOG_ERR("Response does not match expected start of frame");
        return -1;
    }

    // We can check VP if we start using values different then 0x82

    /* Check if the response is the ACK command */
    if (memcmp(&Obj->CmdResp[4], cmd_ack, sizeof(cmd_ack)) != 0)
    {
        DWIN_LOG_ERR("Response does not match expected ACK command");
        return -1;
    }

    return 0;
}

/**
 * @brief   Convert a UTF-8 string to the panel's 8-bit text encoding (Latin-1)
 * @param   in UTF-8 string, null terminated
 * @param   out Output buffer (not null terminated)
 * @param   out_size Size of the output buffer
 * @note    Characters above U+00FF have no glyph in 8-bit mode and become '?'.
 *          Malformed sequences are replaced byte by byte. The text is cut at out_size.
 * @return  Number of bytes written to out
 */
static uint8_t utf8_to_latin1(const char *in, uint8_t *out, uint8_t out_size)
{
    const uint8_t *p = (const uint8_t *)in;
    uint8_t n = 0;

    while ((*p != '\0') && (n < out_size))
    {
        uint32_t cp;

        if (p[0] < 0x80)
        {
            cp = p[0];
            p += 1;
        }
        else if (((p[0] & 0xE0) == 0xC0) && ((p[1] & 0xC0) == 0x80))
        {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        }
        else if (((p[0] & 0xF0) == 0xE0) && ((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80))
        {
            cp = '?'; // U+0800 and above
            p += 3;
        }
        else if (((p[0] & 0xF8) == 0xF0) && ((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80) && ((p[3] & 0xC0) == 0x80))
        {
            cp = '?';
            p += 4;
        }
        else
        {
            cp = '?';
            p += 1;
        }

        out[n++] = (cp <= 0xFF) ? (uint8_t)cp : '?';
    }

    return n;
}
