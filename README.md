# Game Boy emulator for NM-CYD-C5 (ESP32-C5)

Original Game Boy (DMG) emulator running on the ESP32-C5, controlled from your laptop browser over USB.

## Honest status

- Written **without being compiled or run**. The authoring environment had no ESP-IDF toolchain or network.
  The first CI build may need small fixes (most likely Peanut-GB field names in `state_load`).
- Implemented: DMG emulation (Peanut-GB), ROM loading from SD into PSRAM, SRAM saves, experimental save states,
  ROM menu, reset, FPS counter, browser controller (Web Serial), optional video stream to the browser.
- **Not implemented:** sound, Game Boy Color mode. `/roms/gbc` is scanned; GBC-compatible (0x80) games run in
  monochrome DMG mode, GBC-only games are refused. GBC needs a heavier core (e.g. gnuboy) and is a later stage.
- Pins come from public sources (see `main/board.h`), not from your board. Screen orientation, colour inversion
  and RGB order are guesses. Flip `LCD_*` defines in `board.h` if the picture is wrong.

## Build without installing anything (GitHub Actions)

1. Create a new GitHub repository and push this folder (including `.github/`).
2. Open the **Actions** tab, run **Build ESP32-C5 firmware** (it also runs on every push).
3. Download the `gbemu-esp32c5` artifact. It contains:
   - `gbemu-app.bin` - app only (for Launcher from SD, or flashing at 0x10000)
   - `gbemu-merged.bin` - single file, flash at address 0x0
   - `bootloader.bin`, `partition-table.bin` - for the 3-file method
   - `controller.html` - the browser controller

If the build fails, send me the red log lines and I will fix the code.

## Flashing

Single merged file:

    esptool.py --chip esp32c5 -b 460800 write_flash 0x0 gbemu-merged.bin

Three files (C5 bootloader offset is 0x2000):

    esptool.py --chip esp32c5 -b 460800 write_flash 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 gbemu-app.bin

Or load `gbemu-app.bin` from the SD card with your launcher.

## Using it

SD card layout (created automatically if missing):

    /roms/gb/*.gb   /roms/gbc/*.gbc   /saves/gb  /saves/gbc  /states

1. Insert the SD card with ROMs, power the board.
2. Connect the laptop to the **native USB port** (the one that is not the CH340 UART port).
3. Open `controller.html` in Chrome or Edge, click **Connect**, choose the ESP32-C5 port.
   If the browser blocks `file://`, run `python -m http.server` in the folder and open `http://localhost:8000/controller.html`.
4. Click a ROM in the list (or use arrows + Z on the board menu). Play with arrows, Z, X, Enter, Backspace. Esc = menu.

## Known limits and tuning

- Video to the browser is sent at ~30 fps as 2-bit frames; uncheck "Show video" to disable it.
- ROM bytes are read from PSRAM; if speed is short, the next optimisation is caching ROM bank 0 in internal RAM.
- Emulation is frame-paced to 59.7 Hz and drops drawing (not emulation) when late. Real fps is logged on the
  UART console once per second (`emu NN fps, drawn NN fps`).
- Peanut-GB is cloned from its `master` branch at build time; `peanut_version.txt` records the commit.
