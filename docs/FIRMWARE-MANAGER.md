# Firmware manager: design

Status: design, nothing built. Branch to come: `firmware-manager` (from `restore-backup`).
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
- **Current**: the snapshot the unit was last switched to, remembered so the
  auto-save can be named after it ("SeqAdv sequencer").

## 4. SD card layout

```
/fm1/
  units/
    <unit-id>/                 e.g. flash unique ID, or chip key + calibration hash
      unit.txt                 model, chip key, flash ID, first seen, label ("My Elite")
      current.txt              name of the snapshot last loaded
      snapshots/
        0001 SMK37 Open 2026-11-14.bin / .txt
        0002 Official Open 2026-11-20.bin / .txt
        0003 auto SMK37 Open 2026-11-20.bin / .txt
  packages/                    .fwsc files to install (section 6)
  backup-NNN.bin / .txt        existing rescue backups, still listed and importable
  log.txt
```

### Snapshot names

Every snapshot gets a name proposed automatically, **firmware + date**, which
can be edited on the Cardputer keyboard before saving (file names stay
FAT-safe), e.g. `SMK37 Open 2026-11-14` or `FM-1 Midi 2026-11-14`.

- **Firmware part**, best guess first:
  1. the USB product name the unit gave while it was running, before the soft
     key (seen on 2026-11-14: our build `SMK37 Open`, FMini-1 `1209:0001`);
  2. the firmware identity read from the image (as M-Flasher shows it,
     e.g. `SMK-37 Elite 800`, `FM-1 900`);
  3. the name of `current` (the snapshot last loaded).
  Several firmwares share labels (our build reads as "SMK-37 Elite 800"
  in M-Flasher), so the guess is shown for confirmation, never trusted alone.
- **Date part**: the Cardputer ADV has **no real-time clock** (checked in its
  datasheet: no RTC chip, and its power switch cuts everything), so the date
  comes from, in order:
  1. Wi-Fi time (NTP), if `/fm1/wifi.txt` holds a network name and password;
     fetched once per session, a few seconds;
  2. otherwise the date typed once per session, prefilled with the last date
     used (kept on the SD card), so it is usually one ENTER or a small edit.
  The date also goes into the snapshot's `.txt` and the log lines.
- Same name on the same day: a time or a suffix is added (`... 2026-11-14 b`).
- Auto-saves before a LOAD follow the same rule, prefixed `auto`.

## 5. Switching (the main flow)

1. Connect (as today: Elite off, cable, ENTER, Elite on, "Connected"). The unit
   is identified; an unknown unit is offered "add to library" with a label.
2. The library screen lists that unit's snapshots, newest first, with the
   current one marked.
3. Pick one and **LOAD**. The Cardputer shows what will happen, then on confirm:
   1. **Auto-save**: reads the whole flash and stores it as a snapshot named
      after `current` with the date. If the flash equals an existing snapshot,
      no duplicate is kept.
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
- Auto-save before every write. A switch can always be undone by loading the
  auto-save.
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
- UI: a library screen in place of the Advanced mode's single-action keys;
  Guided and Advanced modes stay for rescue.

## 9. Phases

1. **Hardware proof** (no new code): backup and no-op restore with
   `0.2.0-restore.1` on the Elite and on the FM-1.
2. **Library and switching**: unit folders, snapshots with names, auto-save,
   load with boot area kept, import of existing backups. Host tests on the
   simulated unit; then a real switch test: Elite sequencer → official Open →
   sequencer, checking patterns survive; same on the FM-1.
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
