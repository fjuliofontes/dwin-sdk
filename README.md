# DWIN SDK

A portable C driver and **over-the-air asset updater** for DWIN DGUS displays.

There is plenty of Arduino sketch material around for these panels, but very
little you can drop into a real RTOS project, and nothing that rewrites the
display NOR flash at runtime. This SDK does both.

- **Portable core** — `src/dwin.c` and `src/dwin_ota.c` have no RTOS, no
  network stack and no libc beyond `string.h`. Everything the platform provides
  arrives through nine function pointers.
- **Ready made ports** — ESP-IDF and Zephyr, or write the nine callbacks
  yourself in about 80 lines.
- **Display OTA** — stream a new `.icl`, font or icon library into the panel
  from HTTP, MQTT, an SD card, XMODEM, wherever. The chunking, the 32KB block
  commits and the mandatory 6 second flash delay are handled for you.
- **Documented protocol** — see [docs/PROTOCOL.md](docs/PROTOCOL.md) for the
  reverse engineered frame format and the NOR flash write sequence.

Validated on nRF54L15 (Zephyr) in production, and built for ESP32 (ESP-IDF v5.x).

## Layout

```
include/                    public headers
  dwin.h                    display driver
  dwin_ota.h                transport agnostic OTA writer, one DGUS file
  dwin_ota_bundle.h         .dwpk bundle reader, a whole update in one download
  dwin_port.h               optional platform bus IO glue
  dwin_ota_http.h           HTTP(S) OTA client (ESP-IDF)
  dwin_ota_downloader.h     downloader OTA client (Zephyr / nRF Connect)
  dwin_log.h                logging backend selection
src/                        portable core, no platform dependencies
ports/esp-idf/              UART glue + HTTP OTA client
ports/zephyr/               UART glue + downloader OTA client
examples/esp-idf/           dwin_basic, dwin_ota
tools/dwin_pack.py          packs DGUS files into a .dwpk bundle
docs/PROTOCOL.md            the reverse engineered DGUS protocol
```

## Getting started, ESP-IDF

Add the SDK as a component:

```sh
cd <your-project>
git submodule add https://github.com/fjuliofontes/dwin-sdk components/dwin
```

then add `dwin` to your component's `REQUIRES`, and:

```c
#include "dwin.h"
#include "dwin_port.h"

static DWIN_Object_t display;

void app_main(void)
{
    const dwin_port_cfg_t cfg = {
        .uart_port = UART_NUM_1,
        .tx_pin = 17,
        .rx_pin = 16,
        .baud_rate = 115200,
        .rx_buffer_size = 1024,
        .tx_buffer_size = 0,
    };

    dwin_port_init(&display, &cfg);
    vTaskDelay(pdMS_TO_TICKS(3000));   /* the display reboots on init */

    dwin_change_page(&display, 1);
    dwin_write_text_field(&display, 0x2000, "Hello");
    dwin_update_variable(&display, 42, 0x1000);
}
```

Set `CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults`. The driver polls the
UART with 1 ms sleeps while waiting for a reply; at the default 100 Hz tick
every idle poll costs 10 ms.

## Getting started, Zephyr

Add the checkout as a module (`ZEPHYR_EXTRA_MODULES`, or a west manifest entry),
then:

```
CONFIG_DWIN=y
CONFIG_DWIN_PORT=y
```

Give the display UART a `display-uart` alias in your devicetree overlay, or pass
the device to `dwin_port_init()`:

```dts
/ {
    aliases {
        display-uart = &uart1;
    };
};
```

The rest of the API is identical to the ESP-IDF one.

## Display OTA

### One file at a time

```c
#include "dwin_ota.h"

dwin_ota_t ota;
const dwin_ota_cfg_t cfg = {
    .Obj = &display,
    .file_id = 23,          /* 23.icl goes into DGUS file slot 23 */
    .file_size = 262144,
    .nor_flash_size = DWIN_OTA_NOR_FLASH_SIZE_16MB, /* 0 also means 16MB */
    .callback = on_ota_event,
};

dwin_pause_display(&display);

dwin_ota_begin(&ota, &cfg);
while ((n = read_from_wherever(buf, sizeof(buf))) > 0)
{
    dwin_ota_write(&ota, buf, n);   /* any chunk size, the writer re-splits */
}
dwin_ota_finish(&ota);

dwin_reset_display(&display);       /* reboot into the new assets */
```

`dwin_ota_write()` accepts chunks of any size and blocks for about 6 seconds
every 32KB while the panel commits a block to its flash — that delay is imposed
by DGUS, not by this SDK. Budget roughly **70 seconds per 256KB file**.

`CONFIG_DWIN_NOR_FLASH_WAIT_READY` replaces the fixed 6 seconds by polling the
panel until it reports the block as written, which is much faster. It is
experimental, see [docs/PROTOCOL.md](docs/PROTOCOL.md) before enabling it.

### One bundle for the whole update

A display update is usually several DGUS files. `tools/dwin_pack.py` packs them
into a single `.dwpk` bundle, so the device downloads one file:

```
tools/dwin_pack.py pack DWIN_SET --version 1.4.0 --last 22 -o hmi-1.4.0.dwpk
tools/dwin_pack.py list hmi-1.4.0.dwpk
```

The DGUS file number is taken from the leading digits of each file name
(`23.icl`, `22_Config.bin`). The files are written largest first and the one
named by `--last` at the very end: point it at the file that carries your
version, usually 22, so an update that stops half way leaves the old version on
the panel. The packer refuses files that do not fit in the NOR flash or that
run into the slot of another file.

The bundle is not compressed, the panel is the slow part. It starts with a table
(see `include/dwin_ota_bundle.h` for the layout) that the device checks as a
whole, CRC included, before the display is touched:

```c
#include "dwin_ota_bundle.h"

dwin_ota_bundle_t bundle;
const dwin_ota_bundle_cfg_t cfg = {
    .Obj = &display,
    .min_version = running_version, /* refuse a downgrade, NULL accepts anything */
    .callback = on_ota_event,       /* PROGRESS over the whole bundle, FILE_FINISHED */
};

dwin_ota_bundle_begin(&bundle, &cfg);
while ((n = read_from_wherever(buf, sizeof(buf))) > 0)
{
    if (dwin_ota_bundle_write(&bundle, buf, n) < 0)
    {
        break;
    }
}

if (dwin_ota_bundle_finish(&bundle) < 0)   /* resets the display on success */
{
    dwin_ota_bundle_abort(&bundle);        /* brings a paused display back */
}
```

The display is paused by the bundle once the table is accepted and reset by
`dwin_ota_bundle_finish()`. When the source breaks mid way,
`dwin_ota_bundle_rewind()` gives the bundle offset to resume from: the start of
the file that was being written, the ones before it are kept. Every file also
carries a CRC, but the data goes straight into the NOR flash, so a corrupt file
is reported after it was written, not prevented.

The context takes about 700 bytes of RAM with the default of 16 files per bundle
(`CONFIG_DWIN_OTA_BUNDLE_MAX_FILES`).

### With the bundled HTTP client (ESP-IDF)

```c
#include "dwin_ota_http.h"

dwin_ota_http_init(on_ota_event);

const dwin_ota_http_cfg_t cfg = {
    .Obj = &display,
    .url = "https://assets.example.com/display/hmi-1.4.0.dwpk",
    .min_version = running_version,
    .cert_pem = server_root_ca_pem,
};

dwin_ota_http_start(&cfg);
```

The transfer runs in its own task and reports `STARTED`, `PROGRESS`,
`FILE_FINISHED`, `FINISHED`, `ERROR` and `CANCELLED` through the callback. A
broken download is resumed with an HTTP `Range` request.

### With the nRF Connect downloader (Zephyr)

```
CONFIG_DWIN_OTA_DOWNLOADER=y
```

```c
dwin_ota_downloader_init(&display, on_ota_event);
dwin_ota_downloader_start("assets.example.com", "display/hmi-1.4.0.dwpk", running_version, sec_tag, 0, 1024);
```

### Feeding it from anything else

`dwin_ota_bundle_begin/write/finish` for a bundle, or `dwin_ota_begin/write/finish`
for a single DGUS file, is the whole contract. Anything that can produce
bytes — MQTT chunks, an SD card, a USB bulk endpoint, a modem AT command — can
drive a display update with a dozen lines of glue.

## API summary

| Function | What it does |
|---|---|
| `dwin_register_bus_io()` | Bind the nine platform callbacks to an object |
| `dwin_init()` / `dwin_deinit()` | Bring the bus up or down; init soft resets the panel |
| `dwin_change_page()` | Switch the active DGUS page |
| `dwin_get_version()` | Read the GUI and OS firmware versions |
| `dwin_update_variable()` / `dwin_read_variable()` | Write / read one VP word |
| `dwin_read_variables()` / `dwin_write_variables()` | Multi-word read / raw write |
| `dwin_write_text_field()` / `dwin_read_text_field()` | Text widgets |
| `dwin_toggle_ui_element()` | Show or hide a widget through its SP descriptor |
| `dwin_set_color_text()` | Set a widget colour (RGB565) |
| `dwin_change_icon_image()` | Point an icon widget at another icon |
| `dwin_set_brightness()` | Backlight, 0..100 |
| `dwin_update_rtc()` | Push a `struct tm` into the panel RTC |
| `dwin_pause_display()` / `dwin_reset_display()` | Stop the GUI / reboot the panel |
| `dwin_write_data_to_variable_storage()` | Stage 240 bytes in the update space |
| `dwin_write_from_variable_storage_to_nor_flash()` | Commit one 32KB block |

All of them return `0` on success and a negative value on failure.

## Porting to another platform

Implement the nine callbacks of `DWIN_IO_t` and call `dwin_register_bus_io()`.
`ports/esp-idf/dwin_port_esp_idf.c` and `ports/zephyr/dwin_port_zephyr.c` are
both short enough to read as reference implementations.

```c
int8_t   io_init(void);                        /* bring the UART up            */
int8_t   io_deinit(void);                      /* release it                   */
void     io_flush_buffer(void);                /* drop pending RX bytes        */
int16_t  io_send(uint8_t *data, uint16_t len); /* blocking write               */
int16_t  io_receive_one(uint8_t *byte);        /* non-blocking, -1 when empty  */
uint32_t io_get_tick(void);                    /* milliseconds, may wrap       */
void     io_sleep_msec(uint32_t ms);           /* yield                        */
int8_t   io_mutex_lock(uint32_t ms);           /* -1 on timeout                */
void     io_mutex_unlock(void);
```

`io_receive_one()` must return immediately when no byte is available: the core
only sleeps when the bus is idle, so a blocking implementation would slow every
reply down to one byte per millisecond.

On a single threaded target the mutex callbacks can return 0 and do nothing.

For logging, define `DWIN_LOG_CUSTOM` and supply the `DWIN_LOG_*` macros
yourself, or let `include/dwin_log.h` pick ESP-IDF, Zephyr or a `printf`
fallback on its own.

## Compatibility

Developed against DGUS II panels. The VP addresses in section 2 of the protocol
notes are the DGUS II system VPs; DGUS I and the T5L "DGUS Pro" firmware use a
different system VP map, so the widget calls will work but the system ones
(brightness, RTC, page switch, flash commit) may not.

## Licence

MIT. See [LICENSE](LICENSE).
