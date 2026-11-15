# M-Harbour

**Firmware manager and rescue tool for the M-VAVE FM-1 and SMK-37, on an
M5Stack Cardputer ADV.** Keep every firmware you use, with its patches and
patterns, in a library on the SD card and switch between them anywhere, with
no computer; and bring back a unit that no longer starts.

Not made by or associated with M-VAVE.

M-Harbour began as a fork of
[cardputer-synth-rescue](https://github.com/jvitkauskas/cardputer-synth-rescue)
by Julius Vitkauskas (MIT): the rescue is his work, extended. It uses
[M-Flasher](https://github.com/jvitkauskas/m-flasher)'s flash loader and soft
key, by the same author.

## What it does

The Cardputer starts on a home menu with two choices:

- **FIRMWARE MANAGER**, for a unit that works. Connect it by USB-C (on or
  off), the Cardputer asks it over USB MIDI to open its download mode, and
  then:
  - **BACKUP CURRENT**: the whole unit (firmware, presets, settings,
    patterns) into a folder, under a name you type 
  - **LIBRARY**: folders and backups: new folder, rename, move, delete;
  - **LOAD FIRMWARE**: put a backup back on the unit. If what the unit holds
    is not saved yet, it asks first. Only the sectors that differ are written
    and verified; the unit's startup area, factory calibration and Bluetooth
    pairings are never touched.
- **RECOVERY**, for a unit that does not start (a "brick"): the guided mode
  (one step a screen) and the advanced mode (each step under your control),
  which put the unit in its ROM download mode with no computer, back it up,
  and repair it or restore one of its backups.

A backup only ever goes back on the unit it came from (checked by the flash
chip's unique ID and the unit's factory data). A log is kept on the SD card in
`/fm1/log.txt`.

The screens: [docs/mockup/firmware-manager-ui.html](docs/mockup/firmware-manager-ui.html)
(an interactive mockup at the Cardputer's 240 x 135 resolution).

## Install

Download `m-harbour-vX.Y.Z-cardputer-adv.bin` from the
[releases](../../releases): one merged image (bootloader, partition table and
app; it goes at `0x0`).

- **From an SD card, with [M5Launcher](https://github.com/bmorcelli/Launcher)**
  (no computer needed once Launcher is on the Cardputer): copy the `.bin` to
  the card (FAT32, 32 GB or less), open Launcher, choose SD, the file, and
  Install. The same card then holds the backups (`/fm1/`). Launcher's own key
  at power-on gets you back to it.
- **With esptool** from a computer: the Cardputer in download mode (power
  switch OFF, hold G0, plug in USB-C), then
  `esptool --chip esp32s3 write-flash 0x0 m-harbour-vX.Y.Z-cardputer-adv.bin`.

The app takes over the Cardputer's USB-C port for the unit, so to install
something else later from a computer, use download mode again (or Launcher).

## Firmware manager

**Status** (2026-11-15): tested on real units. On an SMK-37 Elite (running
our SeqAdv firmware) and an FM-1 (FMini-1 and Shrub), the Cardputer connected
by itself over USB MIDI every time, backed up into folders, and switched
between firmwares in both directions with their data (about 190 sectors,
20-35 s, verified, startup area kept). Design and test notes:
[docs/FIRMWARE-MANAGER.md](docs/FIRMWARE-MANAGER.md).

**Typical use**: install a new firmware once with its own installer (browser,
phone or M-Flasher), then **BACKUP CURRENT** it into a folder named after it.
From then on, switch between your firmwares with **LOAD FIRMWARE**, anywhere,
without a computer. Save a project on the synth before backing up: only what
is saved in its flash is in a snapshot.

A snapshot is the unit's whole 1 MiB flash with a name: firmware, presets,
settings, patterns. Each unit has its own folder on the card, and in it
folders of your own (one level), e.g. one per firmware:

```
/fm1/units/<unit id>/unit.txt                         model, chip key, flash ID, label
/fm1/units/<unit id>/current.txt                      the snapshot last saved or loaded
/fm1/units/<unit id>/snapshots/0001 First backup.bin / .txt          "(no folder)"
/fm1/units/<unit id>/snapshots/SeqAdv/0003 Dub 1.bin / .txt          folder "SeqAdv"
```

The snapshots directory itself is **(no folder)**: snapshots saved by
earlier versions stay there and stay valid. Folder names are typed by you
and made FAT-safe (`/ \ : * ? " < > |` become `-`, no leading dots, at most
24 characters). The numbers are unique across all of a unit's folders, so a
snapshot keeps its number when it is moved. The unit ID is the flash chip's
unique ID (as `unit id` in a backup's `.txt`), or without one
`k<chip key>-f<CRC32 of the factory data>`. A snapshot's `.txt` is a
backup's `.txt` (the restore's checks read it) plus `name` (exactly as
typed), `date` (empty: the Cardputer has no clock), `model`, `firmware` (the
USB product name the unit gave before the soft key), `source`.

**Connect**: the unit on or off, the USB-C cable to the Cardputer (on
battery), ENTER. No power-on key: the Cardputer becomes the USB host and
waits up to 2 minutes for the unit (switch it on now if it is off). A USB
MIDI device gets the soft key by itself (below) and comes back as the
download mode within 30 s; a unit already in download mode just goes on.
The screen ticks off *Found: <product> (MIDI)*, *Asking it for download
mode*, *Connected · <model>*. Not MIDI, or no answer: "Use Recovery". ESC
cancels (the Cardputer restarts to the connect screen). Only an FM-1 or an
SMK-37 goes on. An SD card is needed.

**Main menu** (`FIRMWARE MANAGER · <MODEL>`): the top line says what the
unit holds: `On device: <folder> / <name> ✓ saved` when its CRC32 (the
chip's, over 1 MiB) equals a snapshot's in any folder, else `not saved in
the library`.

| Key | |
|---|---|
| **1** / ENTER | **BACKUP CURRENT**: pick a folder (`+ New folder...`, `(no folder)`, your folders with their counts; the line above shows the newest name in the highlighted one), then the name window, then the read (checked against the chip's CRC32 and read back): **✓ Saved**, and a note if the same contents are saved under another name |
| **2** | **LIBRARY**: folders (with counts, and `Old backups` if there are `backup-NNN` files), then a folder's snapshots, oldest first, `●` on the one the unit holds |
| **3** | **LOAD FIRMWARE** (below) |
| **I** | **INFO**: device, firmware (the USB product name), what it holds, unit ID, battery, SD card free space and number of snapshots, the app's version; **L** shows the end of `/fm1/log.txt` (`;` `.` scroll, `,` `/` a page) |
| **X** | exit: restarts the unit into its firmware |
| **ESC** | asks: **RESTART DEVICE** or **STAY** |

In the library's folders: **ENTER** opens, **F** new folder, **N** renames
a folder, **D** deletes it (only when empty: it says so otherwise).
`(no folder)` and `Old backups` cannot be renamed or deleted. In a folder:
**N** renames a snapshot (its files and `.txt`), **M** moves it to another
folder (both files renamed into it), **D** deletes it (**DELETE** / **KEEP**;
it warns when it is what the unit holds and has no other copy, or the
unit's only snapshot). In `Old backups`: **I** imports the selected
`backup-NNN` into `(no folder)` (the restore's checks first; it says what the
old file holds compared with the unit now; then a name). Restoring an old
backup as a whole is in RECOVERY, ADVANCED (below).

**The name window** (backup and rename): `In: <folder>` or `Was: <name>`,
the field (prefilled for a backup with the firmware name, e.g. `SMK37 Open`),
and under it the names already in that folder, newest last (with their dates
if they have one). Type (shift for capitals), **BKSP** deletes, **fn+BKSP**
clears, fn+`;` / fn+`.` highlight a name above, **TAB** copies it into the
field to edit, **ENTER** saves, **ESC** cancels. There is no clock: type the
date into the name if you want one.

**LOAD FIRMWARE**:
1. If what the unit holds is not in the library (its CRC32 in no snapshot,
   or the file that has it does not read back as that CRC32): **SAVE
   FIRST?** **YES, SAVE IT** (folder, name, read, then on) or **NO**
   ("unsaved patterns on the device are lost"). If it is saved, no question.
   There is no automatic save any more.
2. A folder, then the snapshot. The one the unit already holds: *Already on
   the device, nothing to write*.
3. The checks (about 10 s): the Cardputer's battery (under 20 % nothing is
   done; not checked without a believable reading), the snapshot's model,
   1 MiB, read twice, the CRC32 in its `.txt`, this unit's (unit ID and
   factory data). Refused: nothing written.
4. **CONFIRM**: checks ✓, the sectors that differ **from 0x4000** with an
   estimated time, what is kept (the startup area `0x0-0x3FFF`; on the
   SMK-37 also the calibration and pairings `0xC0000-0xC1FFF`;
   `0xFF000-0xFFFFF` always). **LOAD** / **CANCEL**. If the snapshot's
   startup area differs, **X** offers writing it too ("exactly", a warning,
   **WRITE ALL** / **CANCEL**).
5. The battery again, then the write (*Do not switch anything off*): only
   differing sectors, each run verified, the whole range at the end, the
   kept areas checked unchanged.
6. **✓ Loaded and verified**: **RESTART DEVICE** (it starts with that
   firmware) or **BACK TO MENU**. `current.txt` is updated.

A load that stops part-way says so; LOAD it again to finish (do not switch
the unit off). Every step is in `/fm1/log.txt`. Untested on hardware:
whether a snapshot taken under one startup area starts on another (stock
bootloader vs m-bootloader); keep the default unless you know.

The advanced mode keeps its own one-screen library (above): ENTER save,
L load (with the same "save first?" question), N rename, D delete, I import,
U label, W rescue, V log, R restart. It lists the snapshots of every folder.

## Recovery

Recovery brings a bricked M-VAVE FM-1 or SMK-37 (JieLi AC791N / WL82) back to life with
nothing but an [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv)
and a USB-C cable: no computer, no extra hardware.

The Cardputer puts the unit into its ROM download mode through its own USB-C
port, becomes the USB host, starts
[M-Flasher](https://github.com/jvitkauskas/m-flasher)'s flash loader (bundled,
MIT, from [m-bootloader](https://github.com/jvitkauskas/m-bootloader)), backs
the whole 1 MiB flash up to the SD card, and installs a fresh
m-bootloader with no firmware: the unit then waits in m-bootloader's MIDI mode
and takes a firmware from that firmware's installer, from a phone or computer.

The rescue is a clean install: the boot area is written and every other sector
up to `0xFEFFF` is erased (no leftovers of earlier firmwares); `0xFF000`-`0xFFFFF`, and on the
SMK-37 `0xC0000`-`0xC1FFF` (Bluetooth pairing, factory calibration), are never
touched. It backs up first (or, without an SD card, warns and goes on only if
told to), writes only what differs, and verifies every run with the chip's
CRC32 and the whole range at the end. Before anything is written the app
identifies the unit with M-Flasher's evidence (the SMK-37's calibration record
at `0xC1224` / `0xFF000`, then the flash ID: FM-1 `856014`/`B36014`, SMK-37
`EB6014`) and installs that board's image. See [rescue/README.md](rescue/README.md).

**Status** (2026-11-12, upstream): works on an FM-1 and an SMK-37 Pro, tested
on real bricks: both with the stock bootloader and their firmware's start
erased (as an interrupted update leaves a unit), and an FM-1 with a firmware
that hangs on its first instruction. Each was backed up and repaired; a repair
takes 2 to 8 s.

A unit whose firmware starts normally does not answer the power-on key (seen
on an SMK-37 Elite and an FM-1, 2026-11-14): that is what the firmware
manager's MIDI connection is for, and the recovery modes offer it too
(the soft key, below).

Files on the SD card: the log in `/fm1/log.txt`, the rescue's backups in
`/fm1/backup-NNN.bin` (with a `.txt` describing the unit), and the firmware
manager's library of named snapshots, in folders, in `/fm1/units/` (above).

## Recovery: use

**Home menu** (what it starts in): **RECOVERY** ("Fix a device that does not
start") or **FIRMWARE MANAGER** ("Back up, keep and switch firmwares").
`;` `.` (or fn+`;` fn+`.`) move, **ENTER** chooses, **1** / **2** are
shortcuts. **RECOVERY** offers **GUIDED** and **ADVANCED** (the same keys;
**ESC**, the `` ` `` key, back to the home menu); both work exactly as
before, and ESC on their first screen comes back to this menu.

The firmware manager's keys everywhere: `;` `.` (or fn+`;` fn+`.`) up and
down, `,` `/` left and right (yes/no boxes), **ENTER** chooses, **ESC**
(`` ` ``, or BKSP) goes back, the letters shown at the bottom of each screen.
Its screens are those of the approved mockup
([docs/mockup/firmware-manager-ui.html](docs/mockup/firmware-manager-ui.html)).

**Guided mode** (RECOVERY, GUIDED): one step a screen, a picture, a few large
words and the key that goes on. Each step has its own key, so a key pressed
twice never skips one; everything else goes on by itself.

1. **ENTER**: start. 2. Switch the synth off, **1**. 3. Connect the USB-C
   cable, **2**. 4. An SD card for a backup (optional: the page shows whether
   one is in), **3**. 5. Switch the synth on: the Cardputer connects and finds
   out which unit it is. 6. The backup (or, without a card, **C** to go on
   without one). 7. **Y**: repair. 8. Done: the unit restarts.

A problem says what to do and has its own "try again" key; **V** shows the
log; **ESC** goes back.

**It started normally**: the synth's own firmware came up instead of the
download mode. If it is a USB MIDI device, **M** asks it over MIDI to switch
(the soft key, below); otherwise, or if it does not answer, switch it off and
**4** to try again.

**Advanced mode** (RECOVERY, ADVANCED, or **A** on the guided mode's first
page): the log on screen and each step under your control:

- **P** / **T** with nothing plugged in: pin check (`H3 L0`) and pulse test
  (about 3000 edges, well under 500 ppm).
- **ENTER**, then switch the unit on: when it says **Connected**, the
  advanced library screen lists the unit's snapshots (all folders) and old
  backups: ENTER saves a snapshot (into "(no folder)"), L loads the one
  selected (an old `backup-NNN`: restores it, below), W installs the rescue,
  R restarts the unit, ESC ends. Before a load, if what the unit holds is not
  in the library, it asks: **S** saves it first, **N** goes on without,
  ESC cancels. A unit that is neither an FM-1 nor an SMK-37 gets ENTER
  backup, R and ESC only.
- **H**: key only; on **MOVE CABLE TO PC**, move the cable to the PC, keeping
  the unit on.
- **Not download mode**: the unit's firmware started instead; the log shows
  its `vid:pid`, product name and whether it is USB MIDI. For a USB MIDI
  device, **S** sends the soft key (below); ESC ends.

Run on battery, with the unit off when you start.

**The soft key** (after [M-Flasher](https://github.com/jvitkauskas/m-flasher),
`src/midi.rs`): M-VAVE's firmwares (FM-1 015, SMK-37 Pro/Elite 016) and
Felucca answer the SysEx `F0 22 24 35 7D F7` on their first MIDI port by
restarting into the ROM's download mode (`4C4A:8057`). A unit that is
running normally ignores the power-on key, so when the Cardputer finds a USB
MIDI device instead of the download mode, it offers to send that message (as
USB-MIDI packets `04 F0 22 24` `07 35 7D F7`, cable 0, to the first
MIDIStreaming interface's bulk OUT endpoint), only when you press the key.
It then waits up to 30 s for the unit to leave USB and come back as the
download mode, and goes on as after the power-on key. Nothing else is sent
to a device that is not the download mode, and nothing is written in this
step. A firmware that does not know the message ignores it: the log then says
so. Every step is in the log (`/fm1/log.txt`). Tested on an SMK-37 Elite
(SeqAdv) and an FM-1 (FMini-1, Shrub).

## Recovery: restore a backup (advanced mode, L)

Puts one of the unit's own backups (`/fm1/backup-NNN.bin`) back: the unit
becomes exactly as it was then, with its own bootloader, firmware and
settings. After the rescue an SMK-37 Elite that took a firmware from MIDI mode
reports itself as a Pro; restoring the backup made before the rescue brings
back the Elite as it was, with only the Cardputer.

1. **Connected**: the old backups are at the bottom of the library screen
   (`old backup-NNN`), newest first; select one (`;` / `.`) and **L**. Each
   says whose it is by the unit ID in its `.txt`: *this unit*, *OTHER unit*,
   *no unit ID* or *no .txt*.
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

**Status**: tested on a PC against a simulated unit (`tests/host/`) and on
real units: the checks and an old backup's restore on an FM-1, the no-op
restore on an SMK-37 Elite (2026-11-14/15).

## How the rescue works

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
| `src/uboot_host.cpp` | USB host, Bulk-Only Transport; the soft key's MIDI transfer |
| `src/usb_midi.cpp` | USB MIDI descriptors and packets (host-tested) |
| `src/jl.cpp`, `src/mflr_loader.h` | ROM and loader protocol; M-Flasher's loader (MIT) |
| `src/writer.cpp` | the rescue install, and a backup's restore |
| `src/restore.cpp` | whether a backup is whole and the connected unit's |
| `src/library.cpp`, `src/switcher.cpp` | the library's names, files and list; the order of a LOAD (host-tested) |
| `src/library_sd.cpp` | the library's files on the SD card |
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
unit, and so do the USB MIDI helpers: `tests/host/run.sh` (any C++17 compiler; CI runs it too).

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

## Cautions

- Measure the Cardputer's USB-C VBUS while on battery. If it outputs 5 V, use a
  cable/adapter without VBUS: the unit should run from its own battery.
- Only the repair (**Y** in the guided mode, **W** in the advanced one), the
  restore (**L** on an old backup, then **W**) and a LOAD (the firmware
  manager's **LOAD**, or **X** and **WRITE ALL**; the advanced library's **L**
  then **W**, or **X** and **Y**) write the unit's flash, after a
  confirmation. The repair and the restore back up first (or, for the
  repair, warn without a card); a LOAD asks to save first when what the unit
  holds is not in the library.
- SD card: FAT32. It can go in at any time; the app mounts it when needed.

## Credits

- [cardputer-synth-rescue](https://github.com/jvitkauskas/cardputer-synth-rescue),
  [M-Flasher](https://github.com/jvitkauskas/m-flasher) and
  [m-bootloader](https://github.com/jvitkauskas/m-bootloader) by Julius
  Vitkauskas (MIT): the rescue, the loader, the soft key.
- Timings after [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter)
  and [flipper-fm1-recovery](https://github.com/merthsoft/flipper-fm1-recovery).
- Built with the help of an AI assistant (Claude).

## Licence

MIT ([LICENSE](LICENSE)), the original copyright kept. The parts from elsewhere (the M-Flasher loader, the
rescue images, the fonts, the libraries linked into the firmware) keep their
own licences: see [THIRD-PARTY.md](THIRD-PARTY.md).
