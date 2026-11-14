// Installs a rescue image (m-bootloader's boot area, see rescue/README.md)
// with M-Flasher's rules, plus a clean install: every writable sector below
// 0xFF000 ends up exactly as the image says, the boot area written,
// everything else erased (no firmware, no leftovers of earlier ones), so
// m-bootloader opens its MIDI mode at the next start and waits for a firmware. Never touched: 0xFF000-0xFFFFF (key_mac, and on the
// SMK-37 m-bootloader's copy of the calibration) and, on the SMK-37,
// 0xC0000-0xC1FFF (Bluetooth pairing, the factory calibration).
//
// The same rules restore a whole backup instead (a Source: the file on the SD
// card, a sector at a time): every writable sector as the backup has it, the
// boot area included, written last.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace writer {

static constexpr uint32_t SECTOR = 0x1000;
static constexpr uint32_t END = 0xFF000;  // first byte never written
static constexpr uint32_t SECTORS = END / SECTOR;
static constexpr uint32_t PIECE = 4;  // sectors of a Source held in RAM while they are written

typedef void (*Log)(const char *fmt, ...);
typedef void (*Progress)(uint32_t done, uint32_t total);

enum Board { FM1, SMK37 };

// A full flash image read a sector at a time (a backup on the SD card), so it
// never has to fit in RAM. plan() notes each sector's CRC32 as it reads it;
// apply() wants the same bytes again, or stops before erasing anything more.
struct Source {
    bool (*read)(void *ctx, uint32_t sector, uint8_t *buf, char *err, size_t errlen);  // SECTOR bytes
    void *ctx;
    uint32_t crc[SECTORS];
};

struct Image {
    const char *name;            // for the screen
    const uint8_t *boot;         // 0x00000-0x03FFF
    uint32_t boot_len;           // as embedded
    uint32_t boot_crc;           // as built (src/rescue_info.h)
    uint32_t keep_lo, keep_hi;   // a protected range below END (empty: equal)
    Source *src;                 // a full image to install instead (boot: unused)
};

const Image &image(Board b);
// A full image from src for board b: the board's protected ranges, nothing else.
Image full_image(Board b, const char *name, Source *src);

// The embedded boot area is intact (their CRC32s match the build's).
bool image_ok(const Image &img, char *err, size_t errlen);

struct Plan {
    bool change[SECTORS];  // sector differs from the image (never a protected one)
    uint32_t to_write;     // changed sectors that hold data in the image
    uint32_t to_erase;     // changed sectors that are blank in the image
    uint32_t runs;
};

// Reads the unit's flash sector by sector and compares it with the image
// (with or without a backup on the SD card).
bool plan(const Image &img, Plan *p, Progress progress, char *err, size_t errlen);

// Notes the chip's CRC32 of every protected range, then erases, programs and
// verifies run by run (a run that does not verify is done once more, then the
// write stops), then checks every written range against the image and every
// protected one against its CRC from before. From a Source: runs go in pieces
// of PIECE sectors, each read whole (and checked against plan()'s CRCs) before
// it is erased, so a read error never leaves a piece erased and unwritten; the
// boot area comes last, so a stop leaves the bootloader there was.
bool apply(const Image &img, const Plan &p, Log log, Progress progress, char *err, size_t errlen);

}  // namespace writer
