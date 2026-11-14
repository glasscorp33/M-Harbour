#include "restore.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace restore {

static constexpr uint32_t SECTOR = writer::SECTOR;
static uint8_t buf[SECTOR], unit[SECTOR];  // a sector of the file, the same of the unit

// ---------------------------------------------------------------------------
// The .txt

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A hex number of 1 to 8 digits, then the end of the line.
static bool hex32(const char *s, uint32_t *v) {
    uint32_t x = 0;
    int n = 0;
    for (; hexval(*s) >= 0; s++, n++) x = x << 4 | hexval(*s);
    while (*s == ' ' || *s == '\r') s++;
    if (n < 1 || n > 8 || (*s && *s != '\n')) return false;
    *v = x;
    return true;
}

void parse_notes(const char *text, Notes *n) {
    memset(n, 0, sizeof *n);
    for (const char *line = text; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : nullptr) {
        const char *colon = strchr(line, ':'), *eol = strchr(line, '\n');
        if (!colon || (eol && colon > eol)) continue;
        size_t klen = colon - line;
        const char *v = colon + 1;
        while (*v == ' ') v++;
        uint32_t x;
        if (klen == 8 && !memcmp(line, "chip key", 8) && hex32(v, &x) && x <= 0xFFFF) {
            n->chip_key = x;
            n->has_key = true;
        } else if (klen == 8 && !memcmp(line, "flash id", 8) && hex32(v, &x)) {
            n->flash_id = x;
            n->has_flash = true;
        } else if (klen == 5 && !memcmp(line, "crc32", 5) && hex32(v, &x)) {
            n->crc = x;
            n->has_crc = true;
        } else if (klen == 4 && !memcmp(line, "size", 4)) {
            char *end;
            unsigned long sz = strtoul(v, &end, 10);
            if (end != v) {
                n->size = sz;
                n->has_size = true;
            }
        } else if (klen == 7 && !memcmp(line, "unit id", 7)) {
            int i = 0;
            for (; i < 8 && hexval(v[2 * i]) >= 0 && hexval(v[2 * i + 1]) >= 0; i++)
                n->unique_id[i] = hexval(v[2 * i]) << 4 | hexval(v[2 * i + 1]);
            const char *rest = v + 16;
            n->has_id = i == 8 && (!*rest || *rest == '\n' || *rest == '\r' || *rest == ' ');
        }
    }
}

// ---------------------------------------------------------------------------
// The check

static bool blank(const uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (d[i] != 0xFF) return false;
    return true;
}

static bool verdict(Result *r, Verdict v, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool verdict(Result *r, Verdict v, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->why, sizeof r->why, fmt, ap);
    va_end(ap);
    r->verdict = v;
    return true;
}

static void hex_id(const uint8_t id[8], char out[17]) {
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", id[i]);
}

bool check(const jl::Info &info, bool smk37, uint32_t size, const Notes *notes, const writer::Source &file,
           writer::Progress progress, Result *r, char *err, size_t errlen) {
    memset(r, 0, sizeof *r);
    // --- the file itself
    if (size != FLASH_SIZE)
        return verdict(r, BAD_FILE, "%lu bytes, not 1048576: not a whole backup", (unsigned long)size);
    if (!notes) return verdict(r, BAD_FILE, "no .txt beside it: nothing to check it by");
    if (!notes->has_key || !notes->has_flash || !notes->has_crc)
        return verdict(r, BAD_FILE, "its .txt lacks the chip key, flash ID or CRC32");
    if (notes->has_size && notes->size != FLASH_SIZE)
        return verdict(r, BAD_FILE, "its .txt says %lu bytes", (unsigned long)notes->size);
    if (notes->chip_key != info.chip_key || notes->flash_id != info.flash_id)
        return verdict(r, OTHER_UNIT, "from chip key %04X flash %06lX, not %04X %06lX", notes->chip_key,
                       (unsigned long)notes->flash_id, info.chip_key, (unsigned long)info.flash_id);

    // Read twice; the second time the factory data are compared with the
    // unit's as they come by.
    uint32_t mac_off = KEY_MAC % SECTOR, mac_len = SECTOR - mac_off;
    bool file_smk = false, trim_same = true, mac_same = false, unique = false;
    uint32_t crc[2];
    for (int pass = 0; pass < 2; pass++) {
        crc[pass] = 0;
        for (uint32_t s = 0; s < FLASH_SIZE / SECTOR; s++) {
            char e[96];
            if (!file.read(file.ctx, s, buf, e, sizeof e))
                return verdict(r, BAD_FILE, "cannot read it at 0x%05lX: %s", (unsigned long)(s * SECTOR), e);
            crc[pass] = jl::crc32_update(crc[pass], buf, SECTOR);
            if (progress && s % 16 == 15) progress(pass * (FLASH_SIZE / SECTOR) + s + 1, 2 * FLASH_SIZE / SECTOR);
            if (pass == 1 && s == 3) r->boot_crc = crc[1];  // 0x0-0x3FFF
            if (pass == 1 && s == USRTRIM / SECTOR) {
                file_smk = !memcmp(buf + 0x224, "trimdat\0", 8);
                if (smk37) {
                    if (!jl::read(USRTRIM, unit, SECTOR, err, errlen)) return false;
                    trim_same = !memcmp(buf, unit, SECTOR);
                    unique |= !blank(unit, SECTOR);
                }
            }
            if (pass == 1 && s == FLASH_SIZE / SECTOR - 1) {
                if (!jl::read(s * SECTOR, unit, SECTOR, err, errlen)) return false;
                mac_same = !memcmp(buf + mac_off, unit + mac_off, mac_len);
                unique |= !blank(unit + mac_off, mac_len);
            }
        }
    }
    r->crc = crc[1];
    if (crc[0] != crc[1])
        return verdict(r, BAD_FILE, "two reads differ (%08lX, %08lX): card?", (unsigned long)crc[0],
                       (unsigned long)crc[1]);
    if (crc[1] != notes->crc)
        return verdict(r, BAD_FILE, "CRC32 %08lX, its .txt says %08lX: damaged", (unsigned long)crc[1],
                       (unsigned long)notes->crc);
    if (file_smk && !smk37) return verdict(r, OTHER_UNIT, "an SMK-37's backup (its calibration), not an FM-1's");

    // --- which unit
    if (notes->has_id && info.has_unique_id) {
        char a[17], b[17];
        hex_id(notes->unique_id, a);
        hex_id(info.unique_id, b);
        if (memcmp(notes->unique_id, info.unique_id, 8))
            return verdict(r, OTHER_UNIT, "unit ID %s, this unit is %s", a, b);
        if (!trim_same || !mac_same)
            return verdict(r, OTHER_UNIT, "same unit ID, but its %s differs from the unit's",
                           !trim_same ? "calibration (0xC1000)" : "key_mac (0xFF23C-)");
        return verdict(r, SAME_BY_ID, "unit ID %s matches", b);
    }
    // no ID on one side (an older loader, or a unit that did not say)
    if (!trim_same) return verdict(r, OTHER_UNIT, "its calibration (0xC1000) is another unit's");
    if (!mac_same) return verdict(r, OTHER_UNIT, "its key_mac (0xFF23C-) is another unit's");
    if (!unique) return verdict(r, CANNOT_TELL, "no unit ID, no factory data: cannot tell whose");
    return verdict(r, SAME_BY_FACTORY, "no unit ID; %s match", smk37 ? "calibration and key_mac" : "key_mac");
}

}  // namespace restore
