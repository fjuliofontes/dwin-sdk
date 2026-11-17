/***********************************************************************************************************************
 *
 * @file    dwin.h
 * @brief   DWIN DGUS display driver header file.
 * @date    07/11/2025
 * @author  Fernando Fontes 
 * @note    Platform Target: portable (ESP-IDF / Zephyr / bare-metal)
 *
 * TAB SIZE: 4 SPACES
 *
 ***********************************************************************************************************************/

#ifndef DWIN_H_
#define DWIN_H_

/****************************************************** Includes ******************************************************/
/* General includes. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************** Defines *******************************************************/
#define DWIN_MFC_SIZE 10
#define DWIN_PROD_ID_SIZE 30

/* The three knobs below can be set from the build system (Kconfig on ESP-IDF and
 * Zephyr, -D on any other platform). */
#if defined(CONFIG_DWIN_CMD_SIZE) && !defined(DWIN_CMD_SIZE)
#define DWIN_CMD_SIZE CONFIG_DWIN_CMD_SIZE
#endif

#if defined(CONFIG_DWIN_RESPONSE_TIMEOUT_MS) && !defined(DWIN_RESPONSE_TIMEOUT_MS)
#define DWIN_RESPONSE_TIMEOUT_MS CONFIG_DWIN_RESPONSE_TIMEOUT_MS
#endif

#if defined(CONFIG_DWIN_MUTEX_TIMEOUT_MS) && !defined(DWIN_MUTEX_TIMEOUT_MS)
#define DWIN_MUTEX_TIMEOUT_MS CONFIG_DWIN_MUTEX_TIMEOUT_MS
#endif

#if defined(CONFIG_DWIN_OTA_WRITE_RETRIES) && !defined(DWIN_OTA_WRITE_RETRIES)
#define DWIN_OTA_WRITE_RETRIES CONFIG_DWIN_OTA_WRITE_RETRIES
#endif

#if defined(CONFIG_DWIN_NOR_FLASH_WAIT_READY) && !defined(DWIN_NOR_FLASH_WAIT_READY)
#define DWIN_NOR_FLASH_WAIT_READY 1
#endif

/* Command response scratch buffer. Must hold the largest expected reply:
 * 6 bytes of framing + 2 bytes per read word. Raise it if multi-word reads of
 * more than 22 words are required. */
#ifndef DWIN_CMD_SIZE
#define DWIN_CMD_SIZE 50
#endif

/* Maximum time, in milliseconds, we wait for a reply from the display. */
#ifndef DWIN_RESPONSE_TIMEOUT_MS
#define DWIN_RESPONSE_TIMEOUT_MS 500
#endif

/* Maximum time, in milliseconds, we wait for the bus mutex. */
#ifndef DWIN_MUTEX_TIMEOUT_MS
#define DWIN_MUTEX_TIMEOUT_MS 100
#endif

/* Times a write to the variable storage update space is sent again when the
 * display does not acknowledge it. Writing the same data to the same address
 * twice is harmless, so a frame lost on the wire does not cost a whole update. */
#ifndef DWIN_OTA_WRITE_RETRIES
#define DWIN_OTA_WRITE_RETRIES 3
#endif

/* How the driver waits for a 32KB block to be committed to the NOR flash.
 *   0: fixed DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS between two commits.
 *   1: poll the display until it reports the commit as done. Much faster, but
 *      experimental: check it on your display before shipping it. */
#ifndef DWIN_NOR_FLASH_WAIT_READY
#define DWIN_NOR_FLASH_WAIT_READY 0
#endif

/* Maximum DGUS payload size of one write. The frame length byte would allow 252
 * bytes, but the T5L only stores the first 248 and still acknowledges the frame. */
#define DWIN_MAX_PAYLOAD_SIZE 248

/****************************************************** Typedefs ******************************************************/
typedef int8_t (*IO_Init_Func)(void);
typedef int8_t (*IO_DeInit_Func)(void);
typedef void (*IO_Flush_Func)(void);
typedef void (*IO_SleepMsec_Func)(uint32_t ms);
typedef int16_t (*IO_Send_Func)(uint8_t *, uint16_t);
typedef int16_t (*IO_ReceiveOne_Func)(uint8_t *pSingleData);
typedef uint32_t (*IO_GetTickCb_Func)(void);
typedef int8_t (*IO_MutexLock_Func)(uint32_t ms);
typedef void (*IO_MutexUnlock_Func)(void);

typedef struct
{
    IO_Init_Func IO_Init;
    IO_DeInit_Func IO_DeInit;
    IO_Flush_Func IO_FlushBuffer;
    IO_Send_Func IO_Send;
    IO_ReceiveOne_Func IO_ReceiveOne;
    IO_GetTickCb_Func IO_GetTickCb;
    IO_SleepMsec_Func IO_SleepMsec;
    IO_MutexLock_Func IO_MutexLock;
    IO_MutexUnlock_Func IO_MutexUnlock;
} DWIN_IO_t;

typedef struct
{
    uint8_t Manufacturer[DWIN_MFC_SIZE];
    uint8_t ProductID[DWIN_PROD_ID_SIZE];
    uint8_t GUISoftwareVersion;
    uint8_t OSSoftwareVersion;
    DWIN_IO_t fops;
    uint8_t CmdResp[DWIN_CMD_SIZE];
    uint16_t CmdRespSize;
    /* Internal bookkeeping for the NOR flash update sequence. Do not touch. */
    uint16_t LastVarStorageWriteSize;
    uint32_t LastNorFlashWriteTick;
    bool NorFlashWritten;
} DWIN_Object_t;

typedef enum
{
    DWIN_NOR_FLASH_PAGE_SIZE = 0x40000,                // 256KB, the granularity of a DGUS file slot
    DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE = 0x8000, // 32KB, the granularity of a single flash write op
    DWIN_VARIABLE_STORAGE_WORD_SIZE = 0x02,
    // 32KB of update space == DWIN_NOR_FLASH_WRITE_OPERATION_PAGE_SIZE
    DWIN_VARIABLE_STORAGE_START_UPDATE_SPACE_ADDRESS = 0x8000,
    DWIN_VARIABLE_STORAGE_END_UPDATE_SPACE_ADDRESS = 0xFFFF,
    DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS = 0x1770, // 6000ms
    // Used instead of the fixed delay when DWIN_NOR_FLASH_WAIT_READY is set
    DWIN_NOR_FLASH_READY_DELAY_FIELD = 0x00FF,    // delay field of the commit command
    DWIN_NOR_FLASH_READY_TIMEOUT_MS = 5000,       // longest a commit may take
    DWIN_NOR_FLASH_READY_POLL_MS = 10,            // pause between two status reads
    DWIN_VARIABLE_STORAGE_FRAGMENT_SIZE = 0x78,             // 120 words == 240 bytes
} dwin_update_definitions_t;

/***************************************************** Constants ******************************************************/

/***************************************************** Variables ******************************************************/

/***************************************************** Functions ******************************************************/
/**
 *
 * @brief   Register the bus IO functions for the DWIN display.
 * @param   Obj       Pointer to the DWIN object.
 * @param   IO_Init   Function to initialize the bus IO.
 * @param   IO_DeInit Function to deinitialize the bus IO.
 * @param   IO_FlushBuffer Function to flush the bus IO buffer.
 * @param   IO_Send     Function to send data to the bus IO.
 * @param   IO_ReceiveOne Function to receive one byte from the bus IO.
 * @param   IO_GetTickCb Function to get the current tick count.
 * @param   IO_SleepMsec Function to sleep for a number of milliseconds.
 * @param   IO_MutexLock Function to lock the mutex.
 * @param   IO_MutexUnlock Function to unlock the mutex.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_register_bus_io(DWIN_Object_t *Obj,
                         IO_Init_Func IO_Init,
                         IO_DeInit_Func IO_DeInit,
                         IO_Flush_Func IO_FlushBuffer,
                         IO_Send_Func IO_Send,
                         IO_ReceiveOne_Func IO_ReceiveOne,
                         IO_GetTickCb_Func IO_GetTickCb,
                         IO_SleepMsec_Func IO_SleepMsec,
                         IO_MutexLock_Func IO_MutexLock,
                         IO_MutexUnlock_Func IO_MutexUnlock);

/**
 *
 * @brief   Initialize the DWIN object.
 * @param   Obj       Pointer to the DWIN object.
 * @note    Performs a soft reset of the display to bring it to a known state.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_init(DWIN_Object_t *Obj);

/**
 *
 * @brief   De-initialize the DWIN object and release the bus.
 * @param   Obj       Pointer to the DWIN object.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_deinit(DWIN_Object_t *Obj);

/**
 *
 * @brief   Change active DGUS page on the DWIN display.
 * @param   Obj       Pointer to the DWIN object.
 * @note    The UART device must be ready and configured.
 * @param   page     Target page ID to switch to.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_change_page(DWIN_Object_t *Obj, uint8_t page);

/**
 *
 * @brief   Get the version of the DWIN display.
 * @param   Obj       Pointer to the DWIN object.
 * @param   gui_version Pointer to the GUI version.
 * @param   os_version Pointer to the OS version.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_get_version(DWIN_Object_t *Obj, uint8_t *gui_version, uint8_t *os_version);

/**
 *
 * @brief   Write a 16-bit value to a DGUS VP (Variable Pointer) address.
 * @param   Obj       Pointer to the DWIN object.
 * @note    The UART device must be ready and configured.
 * @param   value   16-bit value to write to the display.
 * @param   address VP address to write to.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_update_variable(DWIN_Object_t *Obj, uint16_t value, uint16_t address);

/**
 *
 * @brief   Read a 16-bit value from a DGUS VP (Variable Pointer) address.
 * @param   Obj       Pointer to the DWIN object.
 * @param   value     Pointer to the value to read.
 * @param   address   VP address to read from.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_read_variable(DWIN_Object_t *Obj, uint16_t *value, uint16_t address);

/**
 *
 * @brief   Read a number of consecutive 16-bit words from a DGUS VP address.
 * @param   Obj         Pointer to the DWIN object.
 * @param   values      Pointer to the destination array of words.
 * @param   address     VP address to read from.
 * @param   word_count  Number of 16-bit words to read.
 * @note    The reply must fit in DWIN_CMD_SIZE: word_count <= (DWIN_CMD_SIZE - 6) / 2.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_read_variables(DWIN_Object_t *Obj, uint16_t *values, uint16_t address, uint8_t word_count);

/**
 *
 * @brief   Write a raw payload to a DGUS VP address and wait for the ACK.
 * @param   Obj       Pointer to the DWIN object.
 * @param   address   VP address to write to.
 * @param   pData     Pointer to the payload.
 * @param   len       Payload length in bytes (max DWIN_MAX_PAYLOAD_SIZE).
 * @note    Escape hatch for DGUS commands this SDK does not wrap yet.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_write_variables(DWIN_Object_t *Obj, uint16_t address, const uint8_t *pData, uint8_t len);

/**
 *
 * @brief   Update the RTC (Real-Time Clock) on the DWIN display.
 * @param   Obj       Pointer to the DWIN object.
 * @note    The UART device must be ready and configured.
 * @param   tm_buffer Pointer to a tm structure containing the time information.
 * @note    The tm structure must contain valid time data (year, month, day, etc.).
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_update_rtc(DWIN_Object_t *Obj, struct tm *tm_buffer);

/**
 *
 * @brief   Pause the display.
 * @param   Obj       Pointer to the DWIN object.
 * @note    Required before a bulk NOR flash update, so the GUI does not read
 *          from the pages being rewritten.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_pause_display(DWIN_Object_t *Obj);

/**
 *
 * @brief   Reset the display.
 * @param   Obj       Pointer to the DWIN object.
 * @note    No ACK is returned by the display for this command.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_reset_display(DWIN_Object_t *Obj);

/**
 * @brief   Set the brightness of the display.
 * @param   Obj       Pointer to the DWIN object.
 * @param   brightness Brightness value to set.
 * @note    The brightness value must be between 0 and 100.
 * @return  Returns 0 on success, otherwise an error code.
 */
int dwin_set_brightness(DWIN_Object_t *Obj, uint8_t brightness);

/**
 *
 * @brief   Write data to the variable storage update space.
 * @param   Obj       Pointer to the DWIN object.
 * @param   pData     Pointer to the data to write.
 * @param   data_size Size of the data to write.
 * @param   fragment  Fragment index.
 * @note    Fragments must be written in order and every fragment but the last
 *          one of a 32KB block must be exactly
 *          DWIN_VARIABLE_STORAGE_FRAGMENT_SIZE * DWIN_VARIABLE_STORAGE_WORD_SIZE bytes.
 * @note    Sent again up to DWIN_OTA_WRITE_RETRIES times when it is not acknowledged.
 * @return  Returns 0 on success, otherwise an error code.
 **/
int dwin_write_data_to_variable_storage(DWIN_Object_t *Obj, uint8_t *pData, uint16_t data_size, uint16_t fragment);

/**
 *
 * @brief   Commit the 32KB variable storage update space to the NOR flash.
 * @param   Obj       Pointer to the DWIN object.
 * @param   destination_id 32KB destination block ID in the NOR flash.
 * @note    Blocks until DWIN_NOR_FLASH_DELAY_BETWEEN_WRITE_OPERATIONS has elapsed
 *          since the previous commit, or, with DWIN_NOR_FLASH_WAIT_READY, until the
 *          display reports this commit as done.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_write_from_variable_storage_to_nor_flash(DWIN_Object_t *Obj, uint16_t destination_id);

/*
 * @brief   Update the selector on the DWIN display.
 * @param   Obj      Pointer to the DWIN object.
 * @param   address  VP address of the selector.
 * @param   x     VP address of the X position of the selector.
 * @param   y     VP address of the Y position of the selector.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_update_selector(DWIN_Object_t *Obj, uint16_t address, uint16_t x, uint16_t y);

/**
 *
 * @brief   Write text to a text field on the DWIN display.
 * @param   Obj      Pointer to the DWIN object.
 * @param   address  VP address of the text field.
 * @param   text     Text to write to the text field (UTF-8, null terminated).
 * @note    The text is converted to the panel's 8-bit encoding (Latin-1), so accented letters need a
 *          Latin-1 font on the panel. Characters above U+00FF are shown as '?'.
 *          At most DWIN_MAX_PAYLOAD_SIZE - 2 characters fit in one write; longer text is rejected.
 * @return  Returns 0 on success, otherwise an error code.
 *
 **/
int dwin_write_text_field(DWIN_Object_t *Obj, uint16_t address, const char *text);

/**
 * @brief   Read text from a text field on the DWIN display.
 * @param   Obj      Pointer to the DWIN object.
 * @param   address  VP address of the text field.
 * @param   text     Pointer to the text to read from the text field.
 * @param   text_size Size of the text buffer.
 * @note    The text buffer must be large enough to hold the text.
 * @return  Returns 0 on success, otherwise an error code.
 **/
int dwin_read_text_field(DWIN_Object_t *Obj, uint16_t address, char *text, uint8_t text_size);

/**
 * @brief   Show a UI element by binding VP to SP (make it visible).
 * @param   Obj   Pointer to the DWIN object.
 * @param   sp    SP (Start Position) address of the UI element.
 * @param   vp    VP (Variable Pointer) address linked to the UI element.
 * @param   show  True to show the UI element, false to hide it.
 * @note    Sends frame: 5A A5 05 82 <SP_H> <SP_L> <VP_H> <VP_L>.
 * @note    The UART interface must be initialized and ready.
 * @return  Returns 0 on success, otherwise an error code.
 */
int dwin_toggle_ui_element(DWIN_Object_t *Obj, uint16_t sp, uint16_t vp, bool show);

/**
 * @brief   Set the color of a text field on the DWIN display.
 * @param   Obj      Pointer to the DWIN object.
 * @param   sp_address SP address of the text field.
 * @param   color    Color to set the text field to (RGB565).
 * @return  Returns 0 on success, otherwise an error code.
 */
int dwin_set_color_text(DWIN_Object_t *Obj, uint16_t sp_address, uint16_t color);

/**
 * @brief   Change the image displayed by an icon widget.
 * @param   Obj        Pointer to the DWIN object.
 * @param   vp_address VP address bound to the icon widget.
 * @param   image_id   Icon ID inside the icon library.
 * @return  Returns 0 on success, otherwise an error code.
 */
int dwin_change_icon_image(DWIN_Object_t *Obj, uint16_t vp_address, uint16_t image_id);

#ifdef __cplusplus
}
#endif

#endif
