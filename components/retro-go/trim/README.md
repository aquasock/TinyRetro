# Trimmed build: Game Boy, Game Boy Color and NES only

The full retro-core carries SNES, PC Engine, Master System/Game Gear/Coleco,
Atari Lynx and Game & Watch as well. None of them are needed here, and dropping
them frees 404 KB of flash for ROMs.

| | full | trimmed |
| --- | --- | --- |
| `retro-core.bin` | 936,464 B | 522,496 B |
| `launcher.bin` | 1,015,136 B | 1,015,136 B |

## The five files here replace upstream ones

Copy them over the paths they are named after:

| this file | replaces |
| --- | --- |
| `retro-core-CMakeLists.txt` | `retro-core/CMakeLists.txt` |
| `retro-core-main-CMakeLists.txt` | `retro-core/main/CMakeLists.txt` |
| `retro-core-main-main.c` | `retro-core/main/main.c` |
| `launcher-applications.c` | `launcher/main/applications.c` |
| `rg_tool.py.patch` | applies to `rg_tool.py` |

They are whole files rather than patches because three of them are short and the
diff would be harder to read than the result. Each carries a comment naming the
upstream original, so restoring a core means restoring the list and the tab
together.

## Four places have to agree

Trimming a core touches four lists, and missing any one of them fails at build
time in a different way:

1. **`retro-core/CMakeLists.txt`** — `COMPONENTS` selects which emulator
   components are compiled.
2. **`retro-core/main/CMakeLists.txt`** — `COMPONENT_SRCEXCLUDE` drops the front
   ends for the removed cores. `register_component()` globs every `.c` in
   `main/`, so excluding them from `COMPONENTS` alone leaves five files that
   `#include` headers that no longer exist.
3. **`retro-core/main/main.c`** — the `configNs` dispatch otherwise calls
   `snes_main`, `pce_main` and friends, which fail at link time.
4. **`launcher/main/applications.c`** — the tabs. Leaving them in gives a menu
   that offers systems whose core is not built.

## Partition layout

`rg_tool.py`'s `PROJECT_APPS` sizes the app slots; the rest goes to the FAT
volume through `--fatsize`. The launcher sat at 95% of a 1 MB slot, so the
layout was resized for this board:

```
launcher    1,310,720 B  (1.25 MB)   77% used
retro-core    786,432 B  (0.75 MB)   66% used
vfs        13,631,488 B  (13.00 MB)  FAT, ROMs
                        ─────────
image        15.06 MB of 16 MB
```

Build with:

```bash
python3 rg_tool.py build-img --target nano-esp32 --fatsize 13631488
```

`--fatsize` is **bytes**. Upstream passes the value straight to `mkfw.py`, which
does `int(x, 0)` and dies on `8M`.

**Changing the partition table wipes the FAT volume**, so repack before copying
ROMs on rather than after.

## Verified on hardware

After flashing: five tabs (`nes`, `gb`, `gbc`, `favorite`, `recent`), storage
mounted at `/fs`, the Classic Controller still armed, 109 FPS with 215 KB
internal and 7577 KB PSRAM free.

**Not verified: any emulator.** No ROMs are present, so neither core has been
run. Everything above is the launcher and the input path.
