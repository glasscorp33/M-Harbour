#include "library_sd.h"

#include <Arduino.h>
#include <string.h>

#include "sd.h"
#include "ui.h"

namespace libsd {

static constexpr size_t PATH = 128;
static char text[1024];                       // a .txt being read or written
static library::Entry scratch[MAX_ENTRIES];   // the list, for save / auto-save
// two sectors of the unit (its factory data), or a piece of a file being copied
static uint8_t work[2 * writer::SECTOR];

const char *today() { return ""; }

// ---------------------------------------------------------------------------
// Small file helpers

static bool read_text(const char *path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    int n = f.read((uint8_t *)text, sizeof text - 1);
    f.close();
    if (n < 0) return false;
    text[n] = 0;
    return true;
}

static bool write_text(const char *path, const char *s) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        ulog("Cannot create %s", path);
        return false;
    }
    size_t n = strlen(s), w = f.write((const uint8_t *)s, n);
    f.close();
    if (w != n) ulog("SD write failed: %s", path);
    return w == n;
}

static void snap_dir(const Unit &u, char *out, size_t n) { snprintf(out, n, "%s/snapshots", u.dir); }

void bin_path(const Unit &u, const library::Entry &e, char *out, size_t n) {
    if (e.kind == library::LEGACY) snprintf(out, n, "/fm1/backup-%03d.bin", e.number);
    else snprintf(out, n, "%s/snapshots/%s.bin", u.dir, e.base);
}

void txt_path(const Unit &u, const library::Entry &e, char *out, size_t n) {
    if (e.kind == library::LEGACY) snprintf(out, n, "/fm1/backup-%03d.txt", e.number);
    else snprintf(out, n, "%s/snapshots/%s.txt", u.dir, e.base);
}

bool read_meta(const Unit &u, const library::Entry &e, library::Meta *m) {
    char path[PATH];
    txt_path(u, e, path, sizeof path);
    if (!read_text(path)) return false;
    library::parse_meta(text, m);
    return true;
}

// The file name of a directory entry, without its folder.
static const char *leaf(File &f) {
    const char *name = strrchr(f.name(), '/');
    return name ? name + 1 : f.name();
}

// ---------------------------------------------------------------------------
// The unit

bool identify(const jl::Info &info, bool smk37, const char *model, Unit *u, char *err, size_t errlen) {
    memset(u, 0, sizeof *u);
    u->model = model;
    uint32_t hash = 0;
    if (!info.has_unique_id) {
        // no unique ID: the factory data the restore compares make the ID
        uint8_t *trim = work, *last = work + writer::SECTOR;
        if (smk37 && !jl::read(restore::USRTRIM, trim, writer::SECTOR, err, errlen)) return false;
        if (!jl::read(restore::FLASH_SIZE - writer::SECTOR, last, writer::SECTOR, err, errlen)) return false;
        hash = library::factory_hash(smk37 ? trim : nullptr, last);
    }
    library::unit_id(info, hash, u->id, sizeof u->id);
    snprintf(u->dir, sizeof u->dir, "/fm1/units/%s", u->id);
    char path[PATH];
    snprintf(path, sizeof path, "%s/unit.txt", u->dir);
    if (sd_ready() && read_text(path)) {
        library::parse_unit(text, &u->file);
        u->known = true;
    }
    ulog("Library: unit %s (%s)", u->id, u->known ? "known" : "new");
    return true;
}

static bool write_unit(Unit *u) {
    char path[PATH];
    snprintf(path, sizeof path, "%s/unit.txt", u->dir);
    if (!library::format_unit(u->file, text, sizeof text)) return false;
    return write_text(path, text);
}

bool ensure(Unit *u, const jl::Info &info) {
    if (!sd_ready()) {
        ulog("No SD card: insert one (FAT32), try again.");
        return false;
    }
    char path[PATH];
    SD.mkdir("/fm1/units");
    SD.mkdir(u->dir);
    snap_dir(*u, path, sizeof path);
    SD.mkdir(path);
    if (!SD.exists(path)) {
        ulog("Cannot create %s", path);
        return false;
    }
    if (u->known) return true;
    memset(&u->file, 0, sizeof u->file);
    snprintf(u->file.model, sizeof u->file.model, "%s", u->model);
    u->file.chip_key = info.chip_key;
    u->file.flash_id = info.flash_id;
    snprintf(u->file.first_seen, sizeof u->file.first_seen, "%s", today());
    if (!write_unit(u)) return false;
    u->known = true;
    ulog("Library: added unit %s (%s)", u->id, u->model);
    return true;
}

bool set_label(Unit *u, const jl::Info &info, const char *label) {
    if (!ensure(u, info)) return false;
    library::clean_name(label, u->file.label, sizeof u->file.label);
    ulog("Library: unit label \"%s\"", u->file.label);
    return write_unit(u);
}

int current(const Unit &u) {
    char path[PATH];
    snprintf(path, sizeof path, "%s/current.txt", u.dir);
    return read_text(path) ? library::parse_current(text) : 0;
}

bool set_current(const Unit &u, int number, const char *name) {
    char path[PATH];
    snprintf(path, sizeof path, "%s/current.txt", u.dir);
    if (!number) return SD.remove(path) || !SD.exists(path);
    char buf[96];
    if (!library::format_current(number, name, buf, sizeof buf)) return false;
    ulog("Library: current is %04d %s", number, name);
    return write_text(path, buf);
}

// ---------------------------------------------------------------------------
// The list

// The highest snapshot number scan() saw, listed or not (past `max`).
static int highest;

// The next number: one more than the highest file there is, even unlisted.
static int next_number() { return highest < library::NUMBER_MAX ? highest + 1 : 0; }

static int scan(const Unit &u, const jl::Info &info, library::Entry *e, int max, bool legacy) {
    highest = 0;
    if (!sd_ready()) return 0;
    int n = 0;
    char path[PATH];
    snap_dir(u, path, sizeof path);
    File dir = SD.open(path);
    if (dir) {
        // names first, the .txt files once the folder is closed
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            int num;
            char base[sizeof e[0].base];
            bool ok = !f.isDirectory() && library::parse_snapshot_file(leaf(f), ".bin", &num, base, sizeof base);
            f.close();
            if (ok && num > highest) highest = num;
            if (ok && n < max) {
                memset(&e[n], 0, sizeof e[n]);
                e[n].number = num;
                snprintf(e[n].base, sizeof e[n].base, "%s", base);
                n++;
            }
        }
        dir.close();
    }
    for (int i = 0; i < n; i++) {
        char base[sizeof e[0].base];
        snprintf(base, sizeof base, "%s", e[i].base);
        snprintf(path, sizeof path, "%s/snapshots/%s.txt", u.dir, base);
        library::snapshot_entry(e[i].number, base, read_text(path) ? text : nullptr, &e[i]);
    }
    if (legacy) {
        int first = n;
        dir = SD.open("/fm1");
        if (dir) {
            for (File f = dir.openNextFile(); f && n < max; f = dir.openNextFile()) {
                int num;
                bool ok = !f.isDirectory() && library::parse_legacy_file(leaf(f), &num);
                f.close();
                if (ok) e[n++].number = num;
            }
            dir.close();
        }
        for (int i = first; i < n; i++) {
            int num = e[i].number;
            snprintf(path, sizeof path, "/fm1/backup-%03d.txt", num);
            library::legacy_entry(num, read_text(path) ? text : nullptr, info, &e[i]);
        }
    }
    library::sort(e, n, current(u));
    return n;
}

int list(const Unit &u, const jl::Info &info, library::Entry *e, int max) { return scan(u, info, e, max, true); }

// ---------------------------------------------------------------------------
// Snapshots

static bool write_meta(const char *path, const library::Meta &m) {
    if (!library::format_meta(m, text, sizeof text)) return false;
    return write_text(path, text);
}

bool save(Unit *u, const jl::Info &info, const char *name, const char *firmware, library::Source source, int *number,
          engine::Progress progress) {
    if (!ensure(u, info)) return false;
    scan(*u, info, scratch, MAX_ENTRIES, false);
    int num = next_number();
    if (!num) {
        ulog("The library is full (number %d used).", library::NUMBER_MAX);
        return false;
    }
    char base[library::BASE_LEN], bin[PATH], txt[PATH];
    library::base_name(num, name, base, sizeof base);
    snprintf(bin, sizeof bin, "%s/snapshots/%s.bin", u->dir, base);
    snprintf(txt, sizeof txt, "%s/snapshots/%s.txt", u->dir, base);
    if (SD.exists(bin) || SD.exists(txt)) {
        ulog("%s is there already: nothing saved.", bin);
        return false;
    }
    ulog("Saving to %s", bin + 5);
    uint32_t crc;
    bool smk = false;
    if (!engine::read_flash(bin, &crc, &smk, progress)) {
        SD.remove(bin);
        ulog("Not saved.");
        return false;
    }
    static library::Meta m;
    library::meta_for(info, name, today(), u->model, firmware, source, crc, &m);
    if (!write_meta(txt, m)) {
        // without its .txt a snapshot can never be loaded: none at all
        SD.remove(bin);
        SD.remove(txt);
        ulog("Its .txt could not be written: not saved.");
        return false;
    }
    ulog("Saved %s (%s), CRC32 %08lX", base, library::source_name(source), (unsigned long)crc);
    *number = num;
    return true;
}

bool auto_save(Unit *u, const jl::Info &info, const char *firmware, const char *target, char *what, size_t whatlen,
               engine::Progress progress) {
    char err[96];
    uint32_t chip;
    if (!jl::crc32(0, restore::FLASH_SIZE, &chip, err, sizeof err)) {
        ulog("Chip CRC failed: %s", err);
        return false;
    }
    if (!ensure(u, info)) return false;
    int n = scan(*u, info, scratch, MAX_ENTRIES, false);
    int i = library::find_crc(scratch, n, chip);
    if (i >= 0) {
        // the same CRC32 in a .txt: kept only if its file really is that
        char bin[PATH];
        uint32_t file;
        bin_path(*u, scratch[i], bin, sizeof bin);
        if (engine::file_crc(bin, &file) && file == chip) {
            snprintf(what, whatlen, "equals %s", scratch[i].base);
            ulog("Auto-save: the unit equals %s (CRC32 %08lX), not stored again", scratch[i].base,
                 (unsigned long)chip);
            return true;
        }
        ulog("Auto-save: %s's file is not its CRC32: saving anew", scratch[i].base);
    }
    int cur = current(*u);
    const char *cur_name = "";
    for (int k = 0; k < n; k++)
        if (scratch[k].number == cur) cur_name = scratch[k].name;
    char name[library::NAME_CHARS + 1];
    library::auto_name(target, cur_name, firmware, today(), u->model, name, sizeof name);
    int num;
    if (!save(u, info, name, firmware, library::SRC_AUTO, &num, progress)) return false;
    char base[library::BASE_LEN];
    library::base_name(num, name, base, sizeof base);
    snprintf(what, whatlen, "%s", base);
    return true;
}

bool import(Unit *u, const jl::Info &info, const library::Entry &legacy, const char *name, int *number,
            engine::Progress progress) {
    static library::Meta m;
    if (legacy.kind != library::LEGACY || !read_meta(*u, legacy, &m) || !m.notes.has_crc) {
        ulog("backup-%03d: no .txt with a CRC32: not imported.", legacy.number);
        return false;
    }
    if (!ensure(u, info)) return false;
    scan(*u, info, scratch, MAX_ENTRIES, false);
    int num = next_number();
    if (!num) return false;
    char base[library::BASE_LEN], from[PATH], bin[PATH], txt[PATH];
    library::base_name(num, name, base, sizeof base);
    bin_path(*u, legacy, from, sizeof from);
    snprintf(bin, sizeof bin, "%s/snapshots/%s.bin", u->dir, base);
    snprintf(txt, sizeof txt, "%s/snapshots/%s.txt", u->dir, base);
    if (SD.exists(bin)) return false;
    ulog("Importing %s to %s", from + 5, bin + 5);
    File in = SD.open(from, FILE_READ), out = SD.open(bin, FILE_WRITE);
    bool ok = in && out && in.size() == restore::FLASH_SIZE;
    uint32_t crc = 0;
    for (uint32_t a = 0; ok && a < restore::FLASH_SIZE; a += sizeof work) {
        ok = in.read(work, sizeof work) == (int)sizeof work && out.write(work, sizeof work) == sizeof work;
        crc = jl::crc32_update(crc, work, sizeof work);
        if (progress && a % 0x10000 == 0) progress(a + sizeof work, restore::FLASH_SIZE);
    }
    if (in) in.close();
    if (out) out.close();
    uint32_t back = 0;
    if (!ok || crc != m.notes.crc || !engine::file_crc(bin, &back) || back != m.notes.crc) {
        SD.remove(bin);
        ulog("The copy is not the backup (CRC32 %08lX, %08lX, want %08lX): not imported.", (unsigned long)crc,
             (unsigned long)back, (unsigned long)m.notes.crc);
        return false;
    }
    library::clean_name(name, m.name, sizeof m.name);
    snprintf(m.date, sizeof m.date, "%s", today());
    if (!m.model[0]) snprintf(m.model, sizeof m.model, "%s", u->model);
    m.firmware[0] = 0;
    m.source = library::SRC_IMPORT;
    snprintf(m.imported, sizeof m.imported, "backup-%03d", legacy.number);
    m.notes.has_size = true;
    m.notes.size = restore::FLASH_SIZE;
    if (!write_meta(txt, m)) {
        SD.remove(bin);
        SD.remove(txt);
        return false;
    }
    ulog("Imported backup-%03d as %s, CRC32 %08lX", legacy.number, base, (unsigned long)crc);
    *number = num;
    return true;
}

bool rename(const Unit &u, const library::Entry &e, const char *name) {
    static library::Meta m;
    if (e.kind != library::SNAPSHOT || !read_meta(u, e, &m)) {
        ulog("%s: no .txt, not renamed.", e.base);
        return false;
    }
    library::Entry to = e;
    library::base_name(e.number, name, to.base, sizeof to.base);
    char ob[PATH], ot[PATH], nb[PATH], nt[PATH];
    bin_path(u, e, ob, sizeof ob);
    txt_path(u, e, ot, sizeof ot);
    bin_path(u, to, nb, sizeof nb);
    txt_path(u, to, nt, sizeof nt);
    if (strcmp(e.base, to.base)) {
        if (SD.exists(nb) || SD.exists(nt)) {
            ulog("%s is there already: not renamed.", nb);
            return false;
        }
        if (!SD.rename(ob, nb)) {
            ulog("Cannot rename %s", ob);
            return false;
        }
        if (!SD.rename(ot, nt)) {
            SD.rename(nb, ob);
            ulog("Cannot rename %s", ot);
            return false;
        }
    }
    library::clean_name(name, m.name, sizeof m.name);
    if (!write_meta(nt, m)) return false;
    ulog("Renamed %s to %s (\"%s\")", e.base, to.base, m.name);
    if (current(u) == e.number) set_current(u, e.number, m.name);
    return true;
}

bool remove(const Unit &u, const library::Entry &e) {
    if (e.kind != library::SNAPSHOT) return false;
    char b[PATH], t[PATH];
    bin_path(u, e, b, sizeof b);
    txt_path(u, e, t, sizeof t);
    bool ok = SD.remove(b);
    SD.remove(t);
    ulog(ok ? "Deleted %s" : "Cannot delete %s", e.base);
    if (ok && current(u) == e.number) set_current(u, 0, "");
    return ok;
}

}  // namespace libsd
