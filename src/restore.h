// Whether a backup may be restored on the connected unit: a whole, intact
// backup of this very unit. No screen and no SD card here (the file comes
// through a writer::Source), so it builds and is tested on a PC too
// (tests/host/).
//
// Which unit it is, strongest first:
//  - the flash chip's unique ID (M-Flasher's loader, FC42), in the backup's
//    .txt since the first version: the same ID is the same unit, another ID
//    is another unit;
//  - the factory data the restore never writes: on the SMK-37 USRTRIM
//    (0xC1000-0xC1FFF, the factory key and pad calibration, measured on each
//    unit), on both boards key_mac (0xFF23C-0xFFFFF). They must match in any
//    case; without an ID on both sides they decide alone, and only if they
//    hold data (blank on both proves nothing).
// Not compared: BTIF (0xC0000-0xC0FFF, Bluetooth pairings: they change in
// use) and 0xFF000-0xFF23B, where m-bootloader keeps its copy of the SMK-37
// calibration (TRIMCOPY, written by it at its first start).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "jl.h"
#include "writer.h"

namespace restore {

static constexpr uint32_t FLASH_SIZE = 0x100000;
static constexpr uint32_t USRTRIM = 0xC1000, USRTRIM_END = 0xC2000;
static constexpr uint32_t KEY_MAC = 0xFF23C, KEY_MAC_END = 0x100000;  // after m-bootloader's TRIMCOPY slot

// What a backup's .txt says (engine::backup writes it).
struct Notes {
    bool has_key, has_flash, has_id, has_crc, has_size;
    uint16_t chip_key;
    uint32_t flash_id;
    uint8_t unique_id[8];
    uint32_t crc, size;
};
// Reads the .txt's lines ("chip key: 980F", "unit id: 0123...", ...).
void parse_notes(const char *text, Notes *n);

enum Verdict {
    SAME_BY_ID,        // the unique IDs match (and the factory data)
    SAME_BY_FACTORY,   // no ID on one side: the factory data match and hold data
    BAD_FILE,          // not 1 MiB, unreadable, reads differently, not as its .txt
    OTHER_UNIT,        // another chip, another ID or other factory data
    CANNOT_TELL,       // no ID, and nothing unit-unique to compare
};

struct Result {
    Verdict verdict;
    uint32_t crc;      // the file's CRC32
    uint32_t boot_crc; // ... of its boot area (0x0-0x3FFF)
    char why[96];      // for the screen and the log
};

// Reads the whole file twice (it must read the same, and as its .txt says),
// then compares it with the unit (info, and the flash through jl::read).
// smk37: the connected unit is an SMK-37 (USRTRIM is compared too).
// notes: nullptr when the backup has no .txt (refused: nothing to check it by).
// progress: sectors read of the two passes. False with err: the unit could
// not be read (nothing decided).
bool check(const jl::Info &info, bool smk37, uint32_t size, const Notes *notes, const writer::Source &file,
           writer::Progress progress, Result *r, char *err, size_t errlen);

inline bool allowed(Verdict v) { return v == SAME_BY_ID || v == SAME_BY_FACTORY; }

}  // namespace restore
