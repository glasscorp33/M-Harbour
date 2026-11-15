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
    else library::snapshot_path(u.dir, e.folder, e.base, ".bin", out, n);
}

void txt_path(const Unit &u, const library::Entry &e, char *out, size_t n) {
    if (e.kind == library::LEGACY) snprintf(out, n, "/fm1/backup-%03d.txt", e.number);
    else library::snapshot_path(u.dir, e.folder, e.base, ".txt", out, n);
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

// A directory in snapshots/ that is a folder this app could have made (its
// name is its own folder_name; others, ".Trashes" and the like, are left
// alone): copied to out.
static bool app_folder(const char *name, char *out) {
    size_t n = strlen(name);
    char safe[library::FOLDER_CHARS + 1];
    if (n > library::FOLDER_CHARS || !library::folder_name(name, safe, sizeof safe) || strcmp(safe, name))
        return false;
    memcpy(out, name, n + 1);
    return true;
}

// The folders found by the last scan.
static char dirs[library::MAX_FOLDERS][library::FOLDER_CHARS + 1];

// Lists one directory: its snapshot files into e (from n, at most max) and,
// with subdirs, the folders in it into dirs. Returns the new n. Names first,
// the .txt files once the directory is closed (few files open at a time).
static int scan_dir(const Unit &u, const char *folder, library::Entry *e, int n, int max, int *ndirs) {
    char path[PATH];
    if (folder[0]) snprintf(path, sizeof path, "%s/snapshots/%s", u.dir, folder);
    else snap_dir(u, path, sizeof path);
    File dir = SD.open(path);
    if (!dir) return n;
    int first = n;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        int num;
        char base[sizeof e[0].base];
        bool is_dir = f.isDirectory();
        if (is_dir && ndirs && *ndirs < library::MAX_FOLDERS && app_folder(leaf(f), dirs[*ndirs])) (*ndirs)++;
        bool ok = !is_dir && library::parse_snapshot_file(leaf(f), ".bin", &num, base, sizeof base);
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
    for (int i = first; i < n; i++) {
        char base[sizeof e[0].base];
        snprintf(base, sizeof base, "%s", e[i].base);
        library::snapshot_path(u.dir, folder, base, ".txt", path, sizeof path);
        library::snapshot_entry(e[i].number, base, read_text(path) ? text : nullptr, &e[i], folder);
    }
    return n;
}

static int n_dirs;

static int scan(const Unit &u, const jl::Info &info, library::Entry *e, int max, bool legacy) {
    highest = 0;
    n_dirs = 0;
    if (!sd_ready()) return 0;
    int n = scan_dir(u, "", e, 0, max, &n_dirs);
    // every folder, for the numbers even past `max`
    for (int d = 0; d < n_dirs; d++) {
        char folder[library::FOLDER_CHARS + 1];
        snprintf(folder, sizeof folder, "%s", dirs[d]);
        n = scan_dir(u, folder, e, n, max, nullptr);
    }
    char path[PATH];
    if (legacy) {
        int first = n;
        File dir = SD.open("/fm1");
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

int folder_dirs(const Unit &u, char (*out)[library::FOLDER_CHARS + 1], int max) {
    int k = 0;
    if (!sd_ready()) return 0;
    char path[PATH];
    snap_dir(u, path, sizeof path);
    File dir = SD.open(path);
    if (!dir) return 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory() && k < max && k < library::MAX_FOLDERS && app_folder(leaf(f), out[k])) k++;
        f.close();
    }
    dir.close();
    return k;
}

int list(const Unit &u, const jl::Info &info, library::Entry *e, int max) { return scan(u, info, e, max, true); }

// ---------------------------------------------------------------------------
// Snapshots

static bool write_meta(const char *path, const library::Meta &m) {
    if (!library::format_meta(m, text, sizeof text)) return false;
    return write_text(path, text);
}

bool save(Unit *u, const jl::Info &info, const char *folder, const char *name, const char *firmware,
          library::Source source, int *number, uint32_t *out_crc, engine::Progress progress) {
    if (!ensure(u, info)) return false;
    folder = folder ? folder : "";
    if (folder[0]) {
        char dir[PATH];
        snprintf(dir, sizeof dir, "%s/snapshots/%s", u->dir, folder);
        if (!SD.exists(dir)) {
            ulog("No folder %s: nothing saved.", dir + 5);
            return false;
        }
    }
    scan(*u, info, scratch, MAX_ENTRIES, false);
    int num = next_number();
    if (!num) {
        ulog("The library is full (number %d used).", library::NUMBER_MAX);
        return false;
    }
    char base[library::BASE_LEN], bin[PATH], txt[PATH];
    library::base_name(num, name, base, sizeof base);
    library::snapshot_path(u->dir, folder, base, ".bin", bin, sizeof bin);
    library::snapshot_path(u->dir, folder, base, ".txt", txt, sizeof txt);
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
    ulog("Saved %s%s%s (%s), CRC32 %08lX", folder, folder[0] ? "/" : "", base, library::source_name(source),
         (unsigned long)crc);
    *number = num;
    if (out_crc) *out_crc = crc;
    return true;
}

bool file_matches(const Unit &u, const library::Entry &e) {
    if (!e.has_crc) return false;
    char bin[PATH];
    uint32_t file;
    bin_path(u, e, bin, sizeof bin);
    bool ok = engine::file_crc(bin, &file) && file == e.crc;
    if (!ok) ulog("%s: its file is not its CRC32 %08lX", e.base, (unsigned long)e.crc);
    return ok;
}

int saved_as(const Unit &u, const library::Entry *e, int n, uint32_t chip_crc) {
    int bad[8], nbad = 0;
    for (;;) {
        int i = library::saved_as(e, n, chip_crc, bad, nbad);
        if (i < 0 || file_matches(u, e[i])) return i;
        if (nbad == (int)(sizeof bad / sizeof bad[0])) return -1;
        bad[nbad++] = i;
    }
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
    library::snapshot_path(u->dir, "", base, ".bin", bin, sizeof bin);
    library::snapshot_path(u->dir, "", base, ".txt", txt, sizeof txt);
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
        // FAT: a name that differs only in case is the same file
        if (!library::same_folder(e.base, to.base) && (SD.exists(nb) || SD.exists(nt))) {
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

bool move(const Unit &u, const library::Entry &e, const char *to_folder) {
    to_folder = to_folder ? to_folder : "";
    if (e.kind != library::SNAPSHOT) return false;
    if (library::same_folder(e.folder, to_folder)) return true;
    library::Entry to = e;
    snprintf(to.folder, sizeof to.folder, "%s", to_folder);
    char ob[PATH], ot[PATH], nb[PATH], nt[PATH];
    bin_path(u, e, ob, sizeof ob);
    txt_path(u, e, ot, sizeof ot);
    bin_path(u, to, nb, sizeof nb);
    txt_path(u, to, nt, sizeof nt);
    if (to_folder[0]) {
        char dir[PATH];
        snprintf(dir, sizeof dir, "%s/snapshots/%s", u.dir, to_folder);
        if (!SD.exists(dir)) {
            ulog("No folder %s: not moved.", dir + 5);
            return false;
        }
    }
    if (SD.exists(nb) || SD.exists(nt)) {
        ulog("%s is there already: not moved.", nb + 5);
        return false;
    }
    // the .txt goes with its .bin; without a .txt only the .bin moves
    bool has_txt = SD.exists(ot);
    if (!SD.rename(ob, nb)) {
        ulog("Cannot move %s", ob + 5);
        return false;
    }
    if (has_txt && !SD.rename(ot, nt)) {
        SD.rename(nb, ob);
        ulog("Cannot move %s", ot + 5);
        return false;
    }
    ulog("Moved %s from \"%s\" to \"%s\"", e.base, e.folder, to_folder);
    return true;
}

static bool folder_path(const Unit &u, const char *name, char *out, size_t n) {
    return snprintf(out, n, "%s/snapshots/%s", u.dir, name) < (int)n;
}

bool make_folder(Unit *u, const jl::Info &info, const char *typed, char *made, size_t madelen, char *why,
                 size_t whylen) {
    char name[library::FOLDER_CHARS + 1], path[PATH];
    if (!library::folder_name(typed, name, sizeof name)) {
        snprintf(why, whylen, "Type a name for the folder.");
        return false;
    }
    if (!ensure(u, info)) {
        snprintf(why, whylen, "The SD card cannot be written.");
        return false;
    }
    folder_path(*u, name, path, sizeof path);
    if (SD.exists(path)) {
        snprintf(why, whylen, "\"%s\" is there already.", name);
        return false;
    }
    if (!SD.mkdir(path) || !SD.exists(path)) {
        ulog("Cannot create %s", path);
        snprintf(why, whylen, "The SD card cannot be written.");
        return false;
    }
    ulog("Library: folder \"%s\"", name);
    snprintf(made, madelen, "%s", name);
    return true;
}

bool rename_folder(const Unit &u, const char *from, const char *typed, char *made, size_t madelen, char *why,
                   size_t whylen) {
    char name[library::FOLDER_CHARS + 1], op[PATH], np[PATH];
    if (!from || !from[0]) {
        snprintf(why, whylen, "\"(no folder)\" keeps its name.");
        return false;
    }
    if (!library::folder_name(typed, name, sizeof name)) {
        snprintf(why, whylen, "Type a name for the folder.");
        return false;
    }
    folder_path(u, from, op, sizeof op);
    folder_path(u, name, np, sizeof np);
    if (strcmp(from, name)) {
        // another name, or only another case (FAT: the same folder)
        if (!library::same_folder(from, name) && SD.exists(np)) {
            snprintf(why, whylen, "\"%s\" is there already.", name);
            return false;
        }
        if (!SD.rename(op, np)) {
            ulog("Cannot rename %s", op);
            snprintf(why, whylen, "The SD card cannot be written.");
            return false;
        }
        ulog("Library: folder \"%s\" renamed \"%s\"", from, name);
    }
    snprintf(made, madelen, "%s", name);
    return true;
}

bool remove_folder(const Unit &u, const char *name, char *why, size_t whylen) {
    char path[PATH];
    if (!name || !name[0]) {
        snprintf(why, whylen, "\"(no folder)\" cannot be deleted.");
        return false;
    }
    folder_path(u, name, path, sizeof path);
    File dir = SD.open(path);
    if (!dir) {
        snprintf(why, whylen, "The folder cannot be read.");
        return false;
    }
    int files = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        files++;
        f.close();
    }
    dir.close();
    if (files) {
        snprintf(why, whylen, "Not empty: move or delete what is in it first.");
        return false;
    }
    if (!SD.rmdir(path)) {
        ulog("Cannot delete %s", path);
        snprintf(why, whylen, "The SD card cannot be written.");
        return false;
    }
    ulog("Library: folder \"%s\" deleted", name);
    return true;
}

}  // namespace libsd
