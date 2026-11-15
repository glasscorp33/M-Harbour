Status: phase 2 (library and switching) built on branch `firmware-manager`
and proven on an SMK-37 Elite with `0.3.0-fm.x` (results at the end). Version
`0.4.0-fm.1` gives it its own screens (section 11): a home menu with
Recovery and Firmware manager, connection without the power-on key, folders,
and "save first?" in place of the auto-save; **this UI version is
host-tested only, not yet run on a unit**. Phase 3 (`.fwsc` packages) and
Wi-Fi time are not built.
Units: M-VAVE FM-1 and SMK-37 Pro / Elite (owner has an FM-1 and an SMK-37 Elite).

## 1. Idea

A full flash image is the whole instrument: firmware, presets, settings,
patterns and sequences. A library of named images on the Cardputer's SD card is
therefore a library of complete instruments. Switching from one to another saves
the current state first, so nothing is lost and every firmware comes back with
its own data.

Everything is done with the Cardputer alone: the synth is put in the JieLi ROM
download mode exactly as the rescue does today. The same program stays a rescue
tool for a unit that does not start.

## 2. What exists already (checked 2026-11-14)

| Tool | Platform | Units | What it does | Gap for this idea |
|---|---|---|---|---|
| cardputer-synth-rescue (+ our `restore-backup`) | Cardputer ADV | FM-1, SMK-37 | Backup, repair with m-bootloader, restore a backup (ours) | No library, names, switching, or package install |
| M-Flasher | Windows, macOS, Linux | FM-1, SMK-37 | Backup, restore (option: keep this unit's bootloader), install `.fwsc` keeping data, install m-bootloader | Needs a computer; no named library or save-before-switch |
| FM-SWITCH (ocean) | iPhone, iPad, Mac | FM-1 | Swap firmware over USB MIDI from a merged firmware list; on the Mac, whole-flash backup and rescue | FM-1 only; no licence stated |
| FM-1 Swap (HyruleanHero1988) | Android | FM-1 | Installs community firmware over USB MIDI; saves presets/patterns where the firmware allows; previews a package on the fm1-emu emulator before flashing; marks firmware that loses saves on the newer FM-1 flash chip | FM-1 only; data backups per firmware, not whole state; needs a working firmware |
| flipper-fm1-recovery, MvaveFM1Unbricker, jl-uboot-tool | Flipper Zero / PC | FM-1 | Recovery and whole-flash backup/restore | Recovery only |

Nothing found that keeps per-unit snapshots of the whole instrument and swaps
between them, nor anything standalone that does it for the SMK-37.

Ideas worth taking from them: M-Flasher's "keep this unit's bootloader" and
its `.fwsc` install rules; FM-1 Swap's new-flash-chip warning (section 7).

## 3. Concepts

- **Unit**: one physical synth, identified as the restore does (chip key, flash
  ID, flash unique ID, factory data). Each unit has its own folder.
- **Snapshot**: a whole-flash image of that unit (1 MiB) plus a small text file:
  name, date, model, firmware identity string (read from the image), source
  (backup / auto-save / installed package), CRC32, flash unique ID.
- **Current**: the snapshot the unit was last saved as or switched to
  (`current.txt`). What the unit holds is decided by its chip CRC32, not by
  `current` (section 11).
- **Folder**: one level of folders of the user's per unit (e.g. one per
  firmware); the snapshots directory itself is "(no folder)".

## 4. SD card layout

```
/fm1/
  units/
    <unit-id>/                 e.g. flash unique ID, or chip key + calibration hash
      unit.txt                 model, chip key, flash ID, first seen, label ("My Elite")
      current.txt              number (and name) of the snapshot last saved or loaded
      snapshots/               "(no folder)"
        0001 Blank start.bin / .txt
        SeqAdv/                a folder (FAT-safe name typed by the user)
          0002 Dub 1.bin / .txt
          0003 Hip hop 2.bin / .txt
  packages/                    .fwsc files to install (section 6)
  backup-NNN.bin / .txt        existing rescue backups, still listed and importable
  log.txt
```

### Snapshot names

Every backup or SAVE opens a **name window**, prefilled with a suggestion the
user can keep or type over. Names are free text, used like project names:
`Blank start`, `Dub 1`, `Hip hop 2`, `FMini-1 16/09/26`.

- Suggestion: firmware + date when both are known, e.g. `SMK37 Open 14/11/26`.
  Firmware part, best guess first: the USB product name the unit gave while
  running (our build: `SMK37 Open`), else the firmware identity read from the
  image (as M-Flasher shows it), else the name of `current`. Several firmwares
  share labels, so it is only a suggestion.
- Date: the Cardputer ADV has **no real-time clock** (datasheet: no RTC chip;
  the power switch cuts everything). Optional Wi-Fi time (NTP) if
  `/fm1/wifi.txt` holds a network and password; otherwise the suggestion has no
  date and the user types it into the name. No Wi-Fi is required.
- The name is kept exactly as typed in the snapshot's `.txt` and shown in the
  list. The file name is a FAT-safe copy (`/ \ : * ? " < > |` become `-`), with
  the snapshot number in front so equal names never collide.
- Numbers are unique across all of a unit's folders; moving a snapshot to
  another folder renames its two files into it and keeps its number.
- (Until 0.3.0: auto-saves before a LOAD, named `auto before <target>`. Gone
  in 0.4.0: section 11.)

## 5. Switching (the main flow)

1. Connect (as today: Elite off, cable, ENTER, Elite on, "Connected"). The unit
   is identified; an unknown unit is offered "add to library" with a label.
2. The library screen lists that unit's snapshots, newest first, with the
   current one marked.
3. Pick one and **LOAD**. The Cardputer shows what will happen, then on confirm:
   1. **Save first?** (0.4.0; until 0.3.0 an automatic auto-save): if the
      unit's chip CRC32 is in no snapshot (any folder, its file checked),
      the user is asked to save it first (folder and name) or not.
   2. **Checks** on the chosen snapshot: as the restore today (size, two reads,
      CRC, same unit, factory data), plus model match.
   3. **Write** with the restore writer: only differing sectors, never the
      protected ranges, every piece verified, final check.
   4. **Boot area kept by default** (below). Restart the unit.
4. `current` is updated. Log every step.

Other actions on the library screen: **SAVE** (snapshot of the current state,
with a name), **RENAME**, **DELETE** (with confirmation; never the only copy of
a unit's oldest snapshot without a second confirmation), **IMPORT** an existing
`backup-NNN.bin`.

### Boot area policy
Writing the boot area (`0x0-0x3FFF`) is the riskiest write. Default when
switching: keep the unit's current boot area and write `0x4000` upwards only,
as M-Flasher's "keep this unit's bootloader". Writing it is offered only when
the snapshot's boot area differs and the user asks ("restore exactly"), with a
warning. A snapshot whose firmware needs a different boot area than the unit
has (e.g. stock vs m-bootloader configurations, FM-1 015 vs 016) is flagged
from the image's boot table, using M-Flasher's checks as the reference.

## 6. Installing a firmware the unit has never had

From `/fm1/packages/*.fwsc`, following M-Flasher's rules (MIT, same protocol
family): parse the package, check it is for this model, write its flash image
only, keep the boot area and everything outside the package (settings,
pairings). Auto-save first, as in a switch, and offer to save the result as a
new snapshot after the first start. Model rules: an Elite package on an Elite;
FM-1 / MiniFM cross-install warned as M-Flasher does. Our own SMK37 OPEN
builds are `.fwsc` in M-VAVE's format and should install the same way
(to verify).

This is phase 3: switching between snapshots needs no package parser.

## 7. Safety rules

- Protected ranges never written: `0xFF000+` on all units; on the SMK-37 also
  `0xC0000-0xC1FFF`. Bluetooth pairings and calibration are therefore shared by
  all snapshots of a unit, not swapped.
- A snapshot is only ever written to the unit it was taken from.
- Before every LOAD, what the unit holds is either already in the library
  (by its CRC32) or the user is asked to save it first; saying no is an
  explicit choice ("unsaved patterns on the device are lost"). A switch
  can be undone by loading what was saved.
- Every write verified; a stop part-way leaves the boot area untouched, and the
  same load can be run again.
- **Newer FM-1 flash chip** (`0xB36014`, units sold since autumn 2026): some
  firmwares lose saves on it. Show the unit's flash ID, and warn when a snapshot
  or package is known not to support it (FM-1 Swap and the Firmware Hub keep such
  marks; a small table on the SD card, user-editable).
- Flash wear: a switch rewrites at most a few hundred 4 KiB sectors; NOR flash of
  this class is typically rated around 100,000 erase cycles per sector (typical
  figure, not checked for these exact parts). Not a practical limit.
- Battery: refuse to start a write if the Cardputer battery is low; advise the
  synth be charged. A cut mid-write is recoverable by loading again, but avoid it.

## 8. Program structure

Builds on the existing code, no rewrite:
- `writer`: already writes a streamed full image; add a "start at 0x4000" option.
- `restore`: same-unit and file checks, reused for every load.
- New `library` (pure logic, host-testable): unit folders, snapshot metadata,
  naming, de-duplication by CRC, `current`.
- New `fwsc` (phase 3): package parsing and checks, host-testable against real
  packages.
- UI (0.4.0): a home menu (`src/home.cpp`) with Recovery (the guided and
  advanced modes, unchanged; the advanced mode keeps its one-screen
  library) and the firmware manager (`src/fwman.cpp`, drawn by
  `src/fm_ui.cpp` after `docs/mockup/firmware-manager-ui.html`).

## 9. Phases

1. **Hardware proof** (no new code): backup and no-op restore with
   `0.2.0-restore.1` on the Elite and on the FM-1.
2. **Library and switching**: unit folders, snapshots with names, auto-save,
   load with boot area kept, import of existing backups. Host tests on the
   simulated unit; then a real switch test: Elite sequencer → official Open →
   sequencer, checking patterns survive; same on the FM-1.
   *Status (2026-11-14): built (`src/library.*`, `src/switcher.*`,
   `src/library_sd.*`, the library screen in `src/advanced.cpp`, the
   writer's start address); host tests pass; the real switch test is still
   to do. As built: the date is always empty (no clock; NTP is a TODO in
   `libsd::today()`); SAVE and LOAD make the snapshot `current`, an
   auto-save does not; the second DELETE confirmation is for a unit's only
   snapshot; the boot-table flagging of section 5 is not built (only "the
   boot areas differ" is shown); the "add to library" label is optional
   (U), the unit's folder is made at its first SAVE or IMPORT; the battery
   check reads M5Unified's battery voltage (refused under 20 %, skipped
   without a believable reading).*
   *0.4.0-fm.1 (2026-11-15): the firmware manager's own screens off the
   home menu, connection over USB MIDI without the power-on key, folders,
   and "save first?" instead of the auto-save (section 11). In the firmware
   manager a single DELETE confirmation (with a warning for the unit's only
   snapshot, or the one it holds with no other copy); the advanced mode
   keeps its second confirmation. Host-tested; not yet run on a unit.*
3. **Package install** from `.fwsc`.
4. Nice to have: show the firmware identity and version of each snapshot;
   battery checks; export/import a unit's library as one folder; offer the work
   to the upstream author.

## 10. Open questions

- What identity string sits where in each firmware image (for display); read
  it from the app header as M-Flasher does.
- Whether some firmwares keep data in the protected-adjacent areas that a switch
  must also swap (none expected: everything outside the two protected ranges is
  part of the snapshot).
- Elite only: whether loading a snapshot that was taken under m-bootloader onto a
  stock boot area (or the reverse) starts correctly. The boot-area policy and the
  flagging in section 5 exist for this; test before relying on it.
- Timing of a full switch on real hardware (estimate: under a minute including
  power cycling; the backup itself takes about 2 s).

## Hardware results (2026-11-14, SMK-37 Elite, 0.3.0-fm.1)

- Library: SAVE "Test 1" (1 MiB in 3.7 s), LOAD of the identical state: auto-save
  found the equal snapshot, nothing stored or written ("nothing to do").
- Real switch: a project saved on the Elite, then LOAD "Test 1": the auto-save
  kept the new state first (CRC32 CA16BF6B), boot areas equal (C9D8D66E), 2
  sectors written from 0x4000 (0x7C000 project slot, 0xFE000 settings), all
  verified, boot area and protected ranges untouched, 22 s in all. The Elite
  started without the project; loading the auto-save brought it back.
  **Round trip confirmed.**
- Battery reading works (74 %, 3896 mV).
- 0.3.0-fm.2: auto-saves are named "auto before <snapshot being loaded>"
  (what the unit held is unknown); the restart's expected USB error is no
  longer logged.

## 11. The firmware manager's screens (0.4.0-fm.1)

Built after the approved mockup (`docs/mockup/firmware-manager-ui.html`,
240 x 135; DejaVu Sans at 10, 11 and 13 px and M5GFX's 6 x 8 font; ticks,
dots and arrows drawn). Host-tested only (the library and switcher tests;
the screens were checked on a PC against a simulated unit, not on the
device yet).

- **Home**: RECOVERY (GUIDED, ADVANCED: unchanged) or FIRMWARE MANAGER.
- **Connect without the power-on key** (`engine::connect_any`): the USB
  host waits up to 2 min for any device. USB MIDI (a MIDIStreaming bulk
  OUT): the soft key is sent by itself (choosing the firmware manager asks
  for it), then up to 30 s for 4C4A:8057, the loader, the unit. Already in
  download mode: straight on. Otherwise: "Not answering over MIDI. Use
  Recovery." Only an FM-1 or SMK-37 goes on.
- **What the unit holds** is its chip CRC32 (`jl::crc32` over 1 MiB, about
  a second) compared with every snapshot's `.txt` CRC32, in all folders
  (`library::saved_as`). The main menu says `<folder> / <name> ✓ saved` or
  `not saved in the library`; lists mark the equal snapshot with `●`.
- **No auto-save.** LOAD asks **SAVE FIRST?** only when the unit's CRC32 is
  in no snapshot, or the snapshot that has it does not read back as that
  CRC32 (`libsd::saved_as` reads the file). YES: folder and name, saved,
  then on; NO: on, logged. The switcher (`src/switcher.cpp`) has no
  auto-save step any more; everything else is as before: battery, model,
  the restore's checks, boot area kept from 0x4000, the exact mode only
  when the boot areas differ and confirmed twice, protected ranges never
  written, the battery again before writing.
- **Folders** (`library::folder_name`, `folders`, `in_folder`;
  `libsd::make_folder`, `rename_folder`, `remove_folder` (only empty),
  `move`): one level under `snapshots/`; "(no folder)" is `snapshots/`
  itself, where every earlier snapshot stays. Directories that are not
  names this app could have made (`.Trashes`, ...) are not listed.
- **BACKUP CURRENT**: a folder (the newest name in it shown), the name
  window (the names already in that folder, newest last; fn+; / fn+.
  highlight one, TAB copies it), the read; "Saved", with a note when the
  same CRC32 is saved under another name.
- **LIBRARY**: folders (F, N, D), a folder's snapshots oldest first (N, M,
  D), "Old backups" (`backup-NNN`, I imports into "(no folder)").
- **LOAD FIRMWARE**: save first?, folder, snapshot; equal to the unit:
  nothing to write; else the checks, CONFIRM (sectors from 0x4000, an
  estimated time: 3 s + 90 ms a sector written + 45 ms a sector erased, a
  guess to be checked on hardware; kept: startup, and calibration on the
  SMK-37), writing, "Loaded and verified" with RESTART DEVICE / BACK TO
  MENU.
- **INFO**: device, firmware hint, what it holds, unit ID, battery, SD free
  space and the number of snapshots, the version; L: the log's last 6 KB.
- Not in the firmware manager: the unit label (U), the rescue (W), the old
  single-key library; Recovery's advanced mode keeps those. Its LOAD asks
  the same question (S save first, N no, ESC cancel) instead of the
  auto-save.
- To check on hardware: the connection when the unit is already on when
  the USB host starts, and when it is switched on afterwards; that a unit
  in download mode at connect is taken as is; the time estimate.

