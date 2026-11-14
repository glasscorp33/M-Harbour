# Synth rescue for the M5Stack Cardputer ADV

Brings a bricked M-VAVE FM-1 or SMK-37 (JieLi AC791N / WL82) back to life with
nothing but an [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv)
and a USB-C cable: no computer, no extra hardware.

The Cardputer puts the unit into its ROM download mode through its own USB-C
port, becomes the USB host, starts
[M-Flasher](https://github.com/jvitkauskas/m-flasher)'s flash loader (bundled,
MIT, from [m-bootloader](https://github.com/jvitkauskas/m-bootloader)), backs
the whole 1 MiB flash up to the SD card, and installs a fresh
m-bootloader with no firmware: the unit then waits in m-bootloader's MIDI mode
and takes a firmware from that firmware's installer, from a phone or computer.

- **Guided mode** (the default): one step a screen, a picture, a few large
  words and one key a step (below).
- **Advanced mode**: the log on screen, each step under your control,
  self-tests, and a hand-off to a PC for other tools.

The rescue is a clean install: the boot area is written and every other sector
up to `0xFEFFF` is erased (no leftovers of earlier firmwares); `0xFF000`-`0xFFFFF`, and on the
SMK-37 `0xC0000`-`0xC1FFF` (Bluetooth pairing, factory calibration), are never
touched. It backs up first (or, without an SD card, warns and goes on only if
told to), writes only what differs, and verifies every run with the chip's
CRC32 and the whole range at the end. Before anything is written the app
identifies the unit with M-Flasher's evidence (the SMK-37's calibration record
at `0xC1224` / `0xFF000`, then the flash ID: FM-1 `856014`/`B36014`, SMK-37
`EB6014`) and installs that board's image. See [rescue/README.md](rescue/README.md).

A log is kept on the SD card in `/fm1/log.txt`, backups in
`/fm1/backup-NNN.bin` (with a `.txt` describing the unit).

**Status** (2026-11-12): works on an FM-1 and an SMK-37 Pro, tested on real
bricks: both with the stock bootloader and their firmware's start erased (as
an interrupted update leaves a unit: on the SMK-37 only its bootloader's update
loader shows up on USB), and an FM-1 with the stock bootloader and a firmware
that hangs on its first instruction (nothing on USB at all). Each was backed
up and repaired; a backup reads 1 MiB in about 2 s and matches the chip's
CRC32, a repair takes 2 to 8 s.

## Install

Download `synth-rescue-vX.Y.Z-cardputer-adv.bin` from the
[releases](../../releases): one merged image (bootloader, partition table and
app; it goes at `0x0`).

- **From an SD card, with [M5Launcher](https://github.com/bmorcelli/Launcher)**
  (no computer needed once Launcher is on the Cardputer): copy the `.bin` to
  the card (FAT32, 32 GB or less), open Launcher, choose SD, the file, and
  Install. The same card then holds the backups (`/fm1/`). Launcher's own key
  at power-on gets you back to it.
- **With esptool** from a computer: the Cardputer in download mode (power
  switch OFF, hold G0, plug in USB-C), then
  `esptool --chip esp32s3 write-flash 0x0 synth-rescue-vX.Y.Z-cardputer-adv.bin`.

The app takes over the Cardputer's USB-C port for the unit, so to install
something else later from a computer, use download mode again (or Launcher).

## How it works

The USB-C D+/D- are the ESP32-S3's native USB pins (GPIO20/GPIO19). The app
detaches the internal USB PHY and uses them as GPIOs (`src/usbkey.cpp`):

1. Sends the key `0x16EF` (D+ clock, D- data, MSB first) until the ROM answers.
2. ROM ACK (D- held low), then the ROM pulls D+ up.
3. 4 us low pulses on D+ every 1 ms, from the RMT peripheral (hardware-timed).
   The ROM calibrates its USB clock from them.
4. Stops when D+ drops after >= 1 s or after 6 s.

Then the pins go to the ESP32-S3's USB host (`src/uboot_host.cpp`: Bulk-Only
Transport), and `src/jl.cpp` (a port of M-Flasher's `jl.rs`) uploads the
loader through the ROM and reads, erases and writes the flash
(`src/writer.cpp`: the rescue).

| Path | What |
|---|---|
| `src/usbkey.cpp` | USB_KEY, calibration pulses, self-tests |
| `src/uboot_host.cpp` | USB host, Bulk-Only Transport |
| `src/jl.cpp`, `src/mflr_loader.h` | ROM and loader protocol; M-Flasher's loader (MIT) |
| `src/writer.cpp` | the rescue install, and a backup's restore |
| `src/restore.cpp` | whether a backup is whole and the connected unit's |
| `src/engine.cpp` | download mode, connection, the unit's model, backup: both modes' work |
| `src/guided.cpp`, `src/advanced.cpp` | the two modes |
| `src/ui.cpp`, `src/art.cpp`, `art/` | screen, keys, log; the guided mode's pictures |
| `rescue/` | the rescue images (FM-1, SMK-37), their provenance and licences |
| `tools/make_rescue_image.sh` | rebuilds the rescue image with M-Flasher's code |
| `tools/make_art.py` | `art/generated/*.png` to the 100x100 pictures and their table |
| `tools/make_fonts.py` | DejaVu Sans to the guided mode's fonts (`src/ui_fonts.h`) |

Timings follow [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter)
and [flipper-fm1-recovery](https://github.com/merthsoft/flipper-fm1-recovery) v0.7.

## Build and flash

PlatformIO (pioarduino platform, Arduino-ESP32 3.x):

```bash
pio run                      # .pio/build/cardputer-adv/firmware.factory.bin = merged image
pio run -t upload
```

The writer and the restore's checks also build on a PC, with a simulated
unit: `tests/host/run.sh` (any C++17 compiler; CI runs it too).

The stock Arduino-ESP32 libraries are enough. Their ESP-IDF has
`CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` set, which cancels every USB
device unless a filter accepts it: `src/uboot_host.cpp` registers one.

The app takes the USB port away from the USB serial interface, so later uploads
need download mode: power switch OFF, hold G0, plug USB-C into the PC.

The merged `firmware.factory.bin` (flash at `0x0`) also works with M5Launcher
from the SD card, or with esptool.

Releases are made by CI (`.github/workflows/build.yml`): every push builds the
image (an artifact of the run); a tag `vX.Y.Z` that matches `APP_VERSION` in
`platformio.ini` also makes the release, with the image and `SHA256SUMS`. To
release: bump `APP_VERSION`, commit, `git tag vX.Y.Z`, push the tag.

## Use

**Guided mode** (what it starts in): one step a screen, a picture, a few large
words and the key that goes on. Each step has its own key, so a key pressed
twice never skips one; everything else goes on by itself.

1. **ENTER**: start. 2. Switch the synth off, **1**. 3. Connect the USB-C
   cable, **2**. 4. An SD card for a backup (optional: the page shows whether
   one is in), **3**. 5. Switch the synth on: the Cardputer connects and finds
   out which unit it is. 6. The backup (or, without a card, **C** to go on
   without one). 7. **Y**: repair. 8. Done: the unit restarts.

A problem says what to do and has its own "try again" key; **V** shows the
log; **ESC** goes back.

**Advanced mode** (**A** on the first page): the log on screen and each step
under your control:

- **P** / **T** with nothing plugged in: pin check (`H3 L0`) and pulse test
  (about 3000 edges, well under 500 ppm).
- **ENTER**, then switch the unit on: when it says **Connected**, ENTER backs
  up, W installs the rescue, L restores a backup (below), R restarts the
  unit, ESC ends.
- **H**: key only; on **MOVE CABLE TO PC**, move the cable to the PC, keeping
  the unit on.

Run on battery, with the unit off when you start.

## Restore a backup (advanced mode, L)

Puts one of the unit's own backups (`/fm1/backup-NNN.bin`) back: the unit
becomes exactly as it was then, with its own bootloader, firmware and
settings. After the rescue an SMK-37 Elite that took a firmware from MIDI mode
reports itself as a Pro; restoring the backup made before the rescue brings
back the Elite as it was, with only the Cardputer.

1. **Connected**, then **L**: the backups on the card, newest first (1-7 picks
   one, N shows more). Each says whose it is by the unit ID in its `.txt`:
   *this unit*, *OTHER unit*, or *no unit ID*.
2. The app checks the backup and refuses it, with nothing written, unless:
   - it is exactly 1 MiB, reads the same twice, and its CRC32 is the one in
     its `.txt` (a backup without its `.txt` is refused);
   - its chip key and flash ID are the unit's;
   - it is this unit's: the flash chip's unique ID (M-Flasher's loader reads
     it; a backup's `.txt` has it as `unit id` when the loader gave one) is the
     unit's, and the factory data the restore never writes match the unit's:
     on the SMK-37 USRTRIM (`0xC1000`-`0xC1FFF`, the factory key and pad
     calibration), on both `0xFF23C`-`0xFFFFF` (key_mac). Without an ID on one side (an older
     loader, a unit that did not give one) the factory data decide alone,
     and only if they hold something: blank on both proves nothing, and the
     backup is refused.
   Not compared, as they change without the unit being another: Bluetooth
   pairings (BTIF, `0xC0000`-`0xC0FFF` on the SMK-37) and `0xFF000`-`0xFF23B`,
   where m-bootloader keeps its copy of the SMK-37 calibration (it writes it at
   its first start, so it is there after a rescue and not in older backups).
3. It shows what the backup holds (whether its boot area is the rescue's
   m-bootloader or not: pick the one made before the rescue to get the unit's
   own bootloader back) and how many sectors differ. **ENTER** goes on.
4. A backup of the unit as it is now, unless this session made one already
   (then that one is the unit as it was when it was connected).
5. **W** writes: only the sectors that differ, as the rescue does (each run
   verified with the chip's CRC32, then the whole range against the file), the
   boot area last. The backup is read four sectors at a time, each piece
   checked against what was read when the sectors were compared, before it is
   erased: a card that fails or changes stops the write before anything more
   is erased, the bootloader still the one there was. L again, with the card
   back in, finishes it.

Never written, as in the rescue: `0xFF000`-`0xFFFFF`, and on the SMK-37
`0xC0000`-`0xC1FFF`. So the unit keeps its current Bluetooth pairings and
m-bootloader's calibration copy (harmless, nothing else reads it).

**Status**: built, and tested only on a PC against a simulated unit
(`tests/host/`: identical image, differing sectors, protected ranges, other
units, bad files, card errors while writing). Not yet run on a real FM-1 or
SMK-37: the first restore on a real unit is the first test. The SMK-37 Elite
is the same hardware as the Pro (`rescue/README.md`) but has not been tested
with this app either.

## Cautions

- Measure the Cardputer's USB-C VBUS while on battery. If it outputs 5 V, use a
  cable/adapter without VBUS: the unit should run from its own battery.
- Only the repair (**Y** in the guided mode, **W** in the advanced one) and
  the restore (**L**, then **W**, advanced mode) write the unit's flash, after
  a backup (or, for the repair, the no-card warning) and a confirmation.
- SD card: FAT32. It can go in at any time; the app mounts it when needed.

## Licence

MIT ([LICENSE](LICENSE)). The parts from elsewhere (the M-Flasher loader, the
rescue images, the fonts, the libraries linked into the firmware) keep their
own licences: see [THIRD-PARTY.md](THIRD-PARTY.md).
