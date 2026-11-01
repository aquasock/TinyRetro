# TinyRetro — retro-go on the Arduino Nano ESP32

A port of [retro-go](https://github.com/ducalex/retro-go) to the Arduino Nano
ESP32 (ESP32-S3, 16 MB flash, 8 MB Octal PSRAM) driving an ILI9340 320×240
panel, with a Wii Classic Controller over I2C as its input device.

This repository is **not a fork**. It holds only the files this port adds, so
they can be dropped into a checkout of upstream retro-go. That keeps the
lineage honest: everything here is additions to GPL-2.0 code, and none of it
modifies retro-go's own logic beyond one registration line.

## What is here

```
components/retro-go/targets/nano-esp32/
    config.h        the hardware description: pins, panel, input, storage
    env.py          chip type and default apps for rg_tool.py
    sdkconfig       ESP-IDF configuration, taken from the esp32-s3-devkit target
components/retro-go/rg_wiimote.c
components/retro-go/rg_wiimote.h
                    Wii Classic Controller / nunchuck over I2C
components/retro-go/rg_input.c.patch
components/retro-go/config.h.patch
                    the two one-block edits to upstream files
components/retro-go/trim/
                    the trimmed build: Game Boy, Game Boy Color and NES only,
                    plus the 13 MB partition layout. See its README.
```

## Applying it

```bash
git clone https://github.com/ducalex/retro-go.git
cd retro-go
git checkout 4ced120              # the upstream commit this was developed against

cp -r /path/to/TinyRetro/components/retro-go/targets/nano-esp32 \
      components/retro-go/targets/
cp /path/to/TinyRetro/components/retro-go/rg_wiimote.* components/retro-go/
git apply /path/to/TinyRetro/components/retro-go/*.patch

# optional: trim to GB/GBC + NES and a 13 MB ROM volume
cp -r /path/to/TinyRetro/components/retro-go/trim/* /tmp/  # see trim/README.md

python3 rg_tool.py build-img --target nano-esp32 --fatsize 8388608
```

`--fatsize` takes **bytes**, not a suffix: upstream's `rg_tool.py` passes the
string straight to `mkfw.py`, which does `int(x, 0)` and dies on `8M`.

Then flash the resulting image at `0x0`:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 \
    retro-go_*_nano-esp32.img
```

## Wiring

### Panel — Adafruit 2.2" PiTFT HAT (ILI9340)

| signal | Nano pin | GPIO | PiTFT HAT (2×20) |
| --- | --- | --- | --- |
| SCK | D13 | 48 | 23 |
| MOSI | D11 | 38 | 19 |
| MISO | D12 | 47 | 21 |
| CS | D10 | 21 | 24 |
| D/C | **D9** | 18 | **22** (Pi GPIO25) |
| 3V3 | 3V3 | — | 1 |
| 5V | **VBUS** | — | 2 |
| GND | GND | — | 6, 9, 14, 20, 25 |

Three things here cost time and are worth reading before wiring:

- **D/C is pin 22, not 18.** Pin 18 on a Pi header is GPIO24; this HAT routes
  D/C to GPIO25. With D/C on the wrong pin the panel lights up and stays blank,
  because every byte looks like a command.
- **5 V comes from VBUS, not VIN.** The Nano's VIN is a 6–21 V input to its own
  buck converter, and Arduino's datasheet states there is no 5 V pin. The HAT
  has no regulator: 3.3 V feeds the display logic and 5 V feeds only the
  backlight anode, so both rails are required.
- **No reset or backlight wires.** The HAT generates RESET from its own
  supervisor (IC1) and its backlight NPN is pulled up, so it is lit whenever
  5 V is present.

### Input — Adafruit Wii Nunchuck Breakout Adapter

| adapter | Nano | |
| --- | --- | --- |
| VIN | 3V3 | see the note below |
| 3V | — | leave open; it is the adapter's LDO output |
| GND | GND | |
| SCL | A5 (GPIO 12) | hardware I2C |
| SDA | A4 (GPIO 11) | hardware I2C |

The adapter's LDO input is VIN and its output feeds the controller, so VIN is a
regulator input rather than a supply rail. It is fed from 3V3 here, which keeps
the host-side lines at 3.3 V on a chip that is not 5 V tolerant; the cost is
that the LDO runs below dropout and the controller sees about 3.0 V. That has
been reliable on this bench.

## The controller driver

A Wii extension controller answers at `0x52`. It is woken by writing `0x55` to
`0xF0` and `0x00` to `0xFB`, identified by six bytes at `0xFA`, and put in data
format 3 by writing `0x03` to `0xFE`.

Two details are load-bearing:

**The transaction shape is not what `rg_i2c_read()` does.** The extension needs
the register written, a STOP, a pause, then a separate read. `rg_i2c_read()`
issues a repeated START, so this driver talks to the port directly.

**The I2C bus is installed here, not by retro-go.** `rg_i2c_init()` is only
called from retro-go's GPIO-expander path, which this target does not use. The
driver installs the bus itself on first use; without that every transaction
fails with "i2c driver not installed".

The button table below is confirmed against real presses on a genuine Classic
Controller, not taken from a datasheet:

| press | byte | bit | retro-go key |
| --- | --- | --- | --- |
| A | 7 | 4 | `RG_KEY_A` |
| B | 7 | 6 | `RG_KEY_B` |
| d-pad left | 7 | 1 | `RG_KEY_LEFT` |
| d-pad right | 6 | 7 | `RG_KEY_RIGHT` |

The remaining bits follow the sourced format-3 table and use the same layout
(d-pad up on byte 7 bit 0, down on byte 6 bit 6, X on byte 7 bit 3, Y on byte 7
bit 5, L on byte 6 bit 5, R on byte 6 bit 1, minus → Select on byte 6 bit 4,
plus → Start on byte 6 bit 2, Home → Menu).

## Getting ROMs onto the board

retro-go scans `/fs/roms/<short_name>` — `/fs/roms/nes`, `/fs/roms/gb`,
`/fs/roms/gbc`. Anything else is invisible, and a wrong layout looks identical
to a wrong build: the mount succeeds and the tab says "list empty".

**Zips must not use a data descriptor.** `rg_storage_unzip_file()` reads the
local file header and takes both sizes from it:

```c
output_buffer_size = header.uncompressed_size;
stream_remaining   = header.compressed_size;
```

Archives written in streaming style carry `flags & 0x08` and leave both fields
zero in the local header, with the real values only in the central directory.
retro-go then allocates nothing and fails on every ROM with "failed to open the
zip". MiSTer's ROM packs are like this, and so is much else.

`tools/fix_zips.py` rewrites them so the sizes are in the local header and
verifies the result:

```bash
python3 tools/fix_zips.py <rom_dir> out --layout roms \
    --system-map "nes=nes,gb=gb,gbc=gbc" --force
```

It is idempotent on already-good archives, skips multi-entry zips (retro-go
only takes the first match), and fails if a CRC changes.

## State

**Working on hardware:** the 320×240 panel, launcher and tabs, FAT storage, the
Classic Controller, and **NES games at 60 FPS**. Balloon Fight runs at
`FPS:60 (S:30 R:30)` with `BUSY:27%` and 8.1 MB of PSRAM free.

Not working or not verified:

- **No audio yet.** The ESP32-S3 has no internal DAC, so an external I2S decoder
  is required. The target is written for an **Adafruit I2S Stereo Decoder
  (UDA1334A, product 3678)** and the pin defines are already in `config.h`:
  LRC → D5, BCLK → D6, DIN → D7, on 3V3. The part is on order and not fitted,
  so `RG_AUDIO_USE_EXT_DAC` is 0 and the sink is a dummy. Flip it to 1 and
  rebuild once the decoder is wired.

  Three reasons that decoder: no MCLK (its PLL derives the clock from BCLK), it
  accepts 3.3 V logic, and it is a line-level output for an amp and speaker.
  The MAX98357A was the other candidate -- cheaper, mono, drives a speaker
  directly -- and would also work with no code change.
- **Game Boy has not been run.** Only one NES title has been played.
- **SNES-era frame rates are unknown** on this chip; only a 60 FPS-capable NES
  title has been measured.
- **The four on-board PiTFT switches are unmapped.** They would need wires from
  HAT pins 11/13/15/16 to spare Nano pins.

## Licence

retro-go is **GPL-2.0**; this port is a derivative work and carries the same
licence. Upstream's `COPYING` applies to everything here. The driver and target
files were written for this port; the protocol knowledge behind them came partly
from TinyTang's measurement records, which are MIT.
