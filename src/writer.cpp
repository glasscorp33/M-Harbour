#include "writer.h"

#include <stdio.h>
#include <string.h>

#include "jl.h"
#include "rescue_info.h"

extern "C" const uint8_t rescue_fm1_boot[], rescue_fm1_boot_end[];
extern "C" const uint8_t rescue_smk37_boot[], rescue_smk37_boot_end[];

namespace writer {

static constexpr uint32_t FLASH_SIZE = 0x100000;
static uint8_t blank[SECTOR];
static uint8_t sector_buf[SECTOR];
static uint8_t piece_buf[PIECE * SECTOR];  // a Source's sectors, read before they are written

const Image &image(Board b) {
    static const Image fm1 = {
        "FM-1", rescue_fm1_boot, (uint32_t)(rescue_fm1_boot_end - rescue_fm1_boot), RESCUE_FM1_BOOT_CRC32, 0, 0,
        nullptr, 0,
    };
    // BTIF (Bluetooth address, pairings) and USRTRIM (the factory calibration)
    static const Image smk37 = {
        "SMK-37", rescue_smk37_boot, (uint32_t)(rescue_smk37_boot_end - rescue_smk37_boot), RESCUE_SMK37_BOOT_CRC32,
        0xC0000, 0xC2000, nullptr, 0,
    };
    return b == SMK37 ? smk37 : fm1;
}

Image full_image(Board b, const char *name, Source *src, uint32_t start) {
    Image img = image(b);
    img.name = name;
    img.boot = nullptr;
    img.boot_len = img.boot_crc = 0;
    img.src = src;
    img.start = start;
    return img;
}

// Never written: 0xFF000-, the board's range, and below the image's start.
static bool is_protected(const Image &img, uint32_t s) {
    uint32_t a = s * SECTOR;
    return a >= END || a < img.start || (a >= img.keep_lo && a < img.keep_hi);
}

// The image's start is usable: a sector multiple, below the protected range.
static bool start_ok(const Image &img, char *err, size_t errlen) {
    bool keep = img.keep_hi > img.keep_lo;
    if (img.start % SECTOR || img.start >= END || (keep && img.start >= img.keep_lo)) {
        snprintf(err, errlen, "bad start address 0x%05lX", (unsigned long)img.start);
        return false;
    }
    return true;
}

// Sector s of a Source into buf, as plan() read it.
static bool load(const Image &img, uint32_t s, uint8_t *buf, char *err, size_t errlen) {
    if (!img.src->read(img.src->ctx, s, buf, err, errlen)) return false;
    if (jl::crc32_update(0, buf, SECTOR) != img.src->crc[s]) {
        snprintf(err, errlen, "the image's 0x%05lX reads differently than before", (unsigned long)(s * SECTOR));
        return false;
    }
    return true;
}

// The image's bytes for sector s: the boot area, then erased flash; from a
// Source, read into buf. nullptr: it could not be read (err says why).
static const uint8_t *target(const Image &img, uint32_t s, uint8_t *buf, char *err, size_t errlen) {
    if (img.src) return load(img, s, buf, err, errlen) ? buf : nullptr;
    uint32_t a = s * SECTOR;
    if (a < BOOT_AREA) return img.boot + a;
    if (!blank[0]) memset(blank, 0xFF, sizeof blank);
    return blank;
}

static bool is_blank(const uint8_t *d) {
    for (uint32_t i = 0; i < SECTOR; i++)
        if (d[i] != 0xFF) return false;
    return true;
}

static bool target_crc(const Image &img, uint32_t first, uint32_t count, uint32_t *crc, char *err, size_t errlen) {
    *crc = 0;
    for (uint32_t s = first; s < first + count; s++) {
        const uint8_t *t = target(img, s, piece_buf, err, errlen);
        if (!t) return false;
        *crc = jl::crc32_update(*crc, t, SECTOR);
    }
    return true;
}

bool image_ok(const Image &img, char *err, size_t errlen) {
    if (img.boot_len != BOOT_AREA) {
        snprintf(err, errlen, "%s rescue image size is wrong (%lu)", img.name, (unsigned long)img.boot_len);
        return false;
    }
    if (jl::crc32_update(0, img.boot, img.boot_len) != img.boot_crc) {
        snprintf(err, errlen, "the %s rescue image in this app is damaged: reinstall the app", img.name);
        return false;
    }
    return true;
}

bool plan(const Image &img, Plan *p, Progress progress, char *err, size_t errlen) {
    memset(p, 0, sizeof *p);
    if (!start_ok(img, err, errlen)) return false;
    bool prev = false;
    for (uint32_t s = 0; s < SECTORS; s++) {
        bool change = false;
        const uint8_t *t = nullptr;
        if (!is_protected(img, s)) {
            if (!jl::read(s * SECTOR, sector_buf, SECTOR, err, errlen)) return false;
            if (img.src) {
                if (!img.src->read(img.src->ctx, s, piece_buf, err, errlen)) return false;
                img.src->crc[s] = jl::crc32_update(0, piece_buf, SECTOR);
                t = piece_buf;
            } else {
                t = target(img, s, piece_buf, err, errlen);
            }
            change = memcmp(sector_buf, t, SECTOR) != 0;
        }
        p->change[s] = change;
        if (change) {
            if (is_blank(t)) p->to_erase++;
            else p->to_write++;
            // a Source's boot area is a run of its own (written last)
            if (!prev || (img.src && s == BOOT_AREA / SECTOR)) p->runs++;
        }
        prev = change;
        if (s % 16 == 15) progress(s + 1, SECTORS);
    }
    return true;
}

// One run of changed sectors: erase, program, verify with the chip's CRC32.
static bool do_run(const Image &img, uint32_t first, uint32_t count, char *err, size_t errlen) {
    if (!jl::erase(first * SECTOR, count * SECTOR, err, errlen)) return false;
    for (uint32_t s = first; s < first + count; s++) {
        const uint8_t *t = target(img, s, piece_buf, err, errlen);
        if (!t) return false;
        if (!is_blank(t) && !jl::program(s * SECTOR, t, SECTOR, err, errlen)) return false;
    }
    uint32_t chip, want;
    if (!jl::crc32(first * SECTOR, count * SECTOR, &chip, err, errlen)) return false;
    if (!target_crc(img, first, count, &want, err, errlen)) return false;
    if (chip != want) {
        snprintf(err, errlen, "0x%05lX-0x%05lX reads back CRC %08lX, not %08lX", (unsigned long)(first * SECTOR),
                 (unsigned long)((first + count) * SECTOR - 1), (unsigned long)chip, (unsigned long)want);
        return false;
    }
    return true;
}

// One piece of a Source's run, already in piece_buf: erase, program, verify.
static bool do_piece(uint32_t first, uint32_t count, char *err, size_t errlen) {
    if (!jl::erase(first * SECTOR, count * SECTOR, err, errlen)) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *t = piece_buf + i * SECTOR;
        if (!is_blank(t) && !jl::program((first + i) * SECTOR, t, SECTOR, err, errlen)) return false;
    }
    uint32_t chip, want = jl::crc32_update(0, piece_buf, count * SECTOR);
    if (!jl::crc32(first * SECTOR, count * SECTOR, &chip, err, errlen)) return false;
    if (chip != want) {
        snprintf(err, errlen, "0x%05lX-0x%05lX reads back CRC %08lX, not %08lX", (unsigned long)(first * SECTOR),
                 (unsigned long)((first + count) * SECTOR - 1), (unsigned long)chip, (unsigned long)want);
        return false;
    }
    return true;
}

// The changed runs in sectors [lo, hi).
static bool do_runs(const Image &img, const Plan &p, uint32_t lo, uint32_t hi, Log log, Progress progress,
                    uint32_t *done, uint32_t total, char *err, size_t errlen) {
    for (uint32_t s = lo; s < hi;) {
        if (!p.change[s]) {
            s++;
            continue;
        }
        uint32_t first = s;
        while (s < hi && p.change[s]) s++;
        uint32_t count = s - first;
        log("0x%05lX-0x%05lX: %lu sector%s", (unsigned long)(first * SECTOR), (unsigned long)(s * SECTOR - 1),
            (unsigned long)count, count == 1 ? "" : "s");
        if (img.src) {
            for (uint32_t a = first; a < s; a += PIECE) {
                uint32_t n = s - a < PIECE ? s - a : PIECE;
                // all of the piece read and checked before any of it is erased
                for (uint32_t i = 0; i < n; i++)
                    if (!load(img, a + i, piece_buf + i * SECTOR, err, errlen)) return false;
                if (!do_piece(a, n, err, errlen)) {
                    log("Not verified (%s): once more", err);
                    if (!do_piece(a, n, err, errlen)) return false;
                }
                *done += n;
                progress(*done, total);
            }
            continue;
        }
        if (!do_run(img, first, count, err, errlen)) {
            log("Not verified (%s): once more", err);
            if (!do_run(img, first, count, err, errlen)) return false;
        }
        *done += count;
        progress(*done, total);
    }
    return true;
}

// [lo, hi) on the chip against the image.
static bool check_written(const Image &img, uint32_t lo, uint32_t hi, char *err, size_t errlen) {
    uint32_t chip, want;
    if (!jl::crc32(lo, hi - lo, &chip, err, errlen)) return false;
    if (!target_crc(img, lo / SECTOR, (hi - lo) / SECTOR, &want, err, errlen)) return false;
    if (chip != want) {
        snprintf(err, errlen, "final check: 0x%05lX-0x%05lX does not match the image", (unsigned long)lo,
                 (unsigned long)(hi - 1));
        return false;
    }
    return true;
}

bool apply(const Image &img, const Plan &p, Log log, Progress progress, char *err, size_t errlen) {
    if (!start_ok(img, err, errlen)) return false;
    for (uint32_t s = 0; s < img.start / SECTOR; s++)
        if (p.change[s]) {  // a plan made for another start
            snprintf(err, errlen, "the plan writes below 0x%05lX: not this image's", (unsigned long)img.start);
            return false;
        }
    // the protected ranges as they are now: they must read the same afterwards
    bool keep = img.keep_hi > img.keep_lo;
    uint32_t kept_crc = 0, top_crc = 0, below_crc = 0;
    if ((keep && !jl::crc32(img.keep_lo, img.keep_hi - img.keep_lo, &kept_crc, err, errlen)) ||
        !jl::crc32(END, FLASH_SIZE - END, &top_crc, err, errlen) ||
        (img.start && !jl::crc32(0, img.start, &below_crc, err, errlen)))
        return false;

    uint32_t total = p.to_write + p.to_erase, done = 0;
    progress(0, total);
    // a Source's boot area last: a stop on the way leaves the bootloader there was
    uint32_t boot = img.src ? BOOT_AREA / SECTOR : 0;
    if (!do_runs(img, p, boot, SECTORS, log, progress, &done, total, err, errlen) ||
        !do_runs(img, p, 0, boot, log, progress, &done, total, err, errlen))
        return false;

    if (keep) {
        if (!check_written(img, img.start, img.keep_lo, err, errlen) ||
            !check_written(img, img.keep_hi, END, err, errlen))
            return false;
    } else if (!check_written(img, img.start, END, err, errlen)) {
        return false;
    }
    uint32_t crc;
    if (img.start) {
        if (!jl::crc32(0, img.start, &crc, err, errlen)) return false;
        if (crc != below_crc) {
            snprintf(err, errlen, "final check: kept 0x00000-0x%05lX changed", (unsigned long)(img.start - 1));
            return false;
        }
        log("Kept as it was: 0x00000-0x%05lX (not written)", (unsigned long)(img.start - 1));
    }
    if (keep) {
        if (!jl::crc32(img.keep_lo, img.keep_hi - img.keep_lo, &crc, err, errlen)) return false;
        if (crc != kept_crc) {
            snprintf(err, errlen, "final check: protected 0x%05lX-0x%05lX changed", (unsigned long)img.keep_lo,
                     (unsigned long)(img.keep_hi - 1));
            return false;
        }
    }
    if (!jl::crc32(END, FLASH_SIZE - END, &crc, err, errlen)) return false;
    if (crc != top_crc) {
        snprintf(err, errlen, "final check: protected 0x%05lX- changed", (unsigned long)END);
        return false;
    }
    if (keep)
        log("Verified: as the image, 0x%05lX-0x%05lX and 0x%05lX- unchanged", (unsigned long)img.keep_lo,
            (unsigned long)(img.keep_hi - 1), (unsigned long)END);
    else
        log("Verified: 0x%05lX-0x%05lX as the image, 0x%05lX- unchanged", (unsigned long)img.start,
            (unsigned long)(END - 1), (unsigned long)END);
    return true;
}

}  // namespace writer
