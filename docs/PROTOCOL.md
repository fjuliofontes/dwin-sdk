# DGUS protocol notes

Everything here was obtained by reverse engineering the DGUS II serial protocol
against real DWIN panels. It is not an official document, and DWIN may change
any of it in a firmware revision.

## 1. Frame format

Every frame, in both directions, looks like this:

```
5A A5 <len> <cmd> <addr_hi> <addr_lo> <payload ...>
```

| Field      | Size | Notes                                                        |
|------------|------|--------------------------------------------------------------|
| `5A A5`    | 2    | Start of frame                                               |
| `len`      | 1    | Bytes that follow, i.e. `1 + 2 + payload_len`                |
| `cmd`      | 1    | `0x82` write VP, `0x83` read VP                              |
| `addr`     | 2    | VP address, big endian, **word** addressed                   |
| `payload`  | n    | Command data                                                 |

There is no checksum in the default DGUS configuration.

Because `len` is a single byte, one frame carries at most **252 payload bytes**.

### Write acknowledgement

A `0x82` write is answered with:

```
5A A5 03 82 4F 4B          ; "OK"
```

The one exception is the display reset, which reboots before it can answer.

### Read reply

A `0x83` read of `N` words is answered with:

```
5A A5 <len> 83 <addr_hi> <addr_lo> <N> <D0_hi> <D0_lo> ... 
```

so the reply is `7 + 2 * N` bytes long.

## 2. System VP addresses used by this SDK

| VP       | Purpose                                        |
|----------|------------------------------------------------|
| `0x0004` | Reset the display, payload `55 AA 5A A5`       |
| `0x000F` | GUI version (high byte) / OS version (low byte)|
| `0x0010` | RTC, 8 byte payload                            |
| `0x0082` | Backlight brightness, 0..100                   |
| `0x0084` | Page switch, payload `5A 01 00 <page>`         |
| `0x00AA` | NOR flash read/write operation (see below)     |
| `0x00FC` | Pause the GUI, payload `55 AA 5A A5`           |

The RTC payload is:

```
<year-2000> <month> <day> <weekday> <hour> <minute> <second> 00
```

## 3. Writing assets to the NOR flash at runtime

This is the part that makes display OTA possible. DGUS exposes a two stage path:

1. **Stage the data** in the *variable storage update space*, a 32KB scratch
   area that starts at word address `0x8000`.
2. **Commit** the whole 32KB area into one NOR flash block with a VP `0x00AA`
   command.

### 3.1 Staging

A normal `0x82` write to `0x8000 + fragment * 0x78` stages 240 bytes
(`0x78` words). 240 bytes is the largest payload that keeps the frame under the
252 byte limit while staying word aligned.

```
5A A5 F3 82 80 00 <240 bytes>      ; fragment 0
5A A5 F3 82 80 78 <240 bytes>      ; fragment 1
...
```

The 32KB area is filled by **136 fragments of 240 bytes plus one final fragment
of 128 bytes**:

```
136 * 240 + 128 = 32768
```

Fragments must be written in order. The display does not report staging
progress, so the writer has to track the fragment index itself.

### 3.2 Committing

Once the update space is full, VP `0x00AA` copies it into the NOR flash:

```
5A A5 0F 82 00 AA 5A 02 <dst_hi> <dst_lo> 80 00 17 70 00 00 00 00
                        ^^ ^^     ^^^^^^^ ^^^^^ ^^^^^
                        |  |      |       |     |
                        |  |      |       |     +-- delay, 0x1770 == 6000 ms
                        |  |      |       +-------- source word address, 0x8000
                        |  |      +---------------- destination 32KB block id
                        |  +----------------------- 0x02: write a 32KB data block
                        +-------------------------- 0x5A: enable external memory op
```

After the ACK, VP `0x00AA` can be read back: the high byte drops from `0x5A` to
`0x00` when the operation has completed.

**The 6 second delay between two commits is mandatory.** Issuing the next
commit early corrupts the flash content. The SDK enforces it by stamping the
tick of the last commit on the object and sleeping the remainder.

### 3.3 Block addressing

The NOR flash is organised in **256KB file slots**, one per DGUS file, and each
slot is 8 consecutive 32KB blocks. So the destination block id of page `p` of
file `N` is:

```
dst = N * 8 + p
```

This is why the file name carries the slot number: `23.icl` goes to file slot
23, i.e. blocks 184 to 191.

### 3.4 Full sequence

```
dwin_pause_display()                  ; stop the GUI from reading the slots
for each file:
    for each 32KB block:
        136 x stage 240 bytes
        1   x stage 128 bytes
        commit block (N * 8 + p)
        wait 6 s
dwin_reset_display()                  ; reboot into the new assets
```

A file shorter than a multiple of 32KB simply commits a partially refreshed
final block; DGUS always transfers the full 32KB.

### 3.5 Timing

One 32KB block costs roughly 6 seconds of mandatory delay plus the UART time of
32KB (about 2.8 s at 115200 baud, 8N1). A full 256KB `.icl` file therefore
takes about 70 seconds. Raising the display baud rate helps with the transfer
but not with the flash delay.

## 4. Things worth knowing

- The display must be **paused** (`VP 0x00FC`) before rewriting slots that the
  running GUI reads from, otherwise it renders garbage or locks up.
- The display **reboots** after a reset command and needs a couple of seconds
  before it answers again.
- Strings written to a text VP are terminated with `FF FF`.
- Hiding a widget is done by pointing its SP descriptor at `FF00` instead of a
  real VP; showing it again restores the VP.
- A widget's colour attribute lives 3 words after its SP descriptor base.
