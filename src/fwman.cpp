// The firmware manager: the screens of docs/mockup/firmware-manager-ui.html
// on the device. What it does with the unit is the engine's, the library's
// (libsd) and the switcher's, as proven on hardware; this file is the flow.
//
//   CONNECT       ENTER: the USB host, then any device (no power-on key):
//   CONNECTING    USB MIDI: the soft key by itself, then the download mode;
//                 already the download mode: on; the loader, which unit
//   main menu     BACKUP CURRENT, LIBRARY, LOAD FIRMWARE; I info, X exit
//                 and restart the device, ESC asks (restart / stay)
#include "fwman.h"

#include <Arduino.h>

#include "engine.h"
#include "fm_ui.h"
#include "library.h"
#include "library_sd.h"
#include "modes.h"
#include "restore.h"
#include "sd.h"
#include "switcher.h"
#include "ui.h"
#include "writer.h"

using engine::unit;
using namespace fmui;

namespace fwman {

static constexpr uint32_t DEVICE_WAIT_MS = 120000;  // for a device to appear on USB
static constexpr int BUSY_BAR_Y = 40;               // the progress bar of the busy screens

static jl::Info info;
static char fw_hint[48];  // the USB product name before the soft key, or ""
static libsd::Unit lu;
static library::Entry ent[libsd::MAX_ENTRIES];
static int n_ent;
static char dirs[library::MAX_FOLDERS][library::FOLDER_CHARS + 1];
static library::Folder fold[library::MAX_FOLDERS + 1];
static int n_fold;
static uint32_t dev_crc;  // what the device holds: its chip's CRC32
static bool dev_crc_ok;
static Row row_buf[libsd::MAX_ENTRIES + library::MAX_FOLDERS + 2];
static int idx_buf[libsd::MAX_ENTRIES];

// s copied into out (n bytes), cut if longer.
static void put(char *out, size_t n, const char *s) {
    if (!n) return;
    size_t k = strnlen(s, n - 1);
    memcpy(out, s, k);
    out[k] = 0;
}

static bool is_smk37() { return unit.model == engine::MODEL_SMK37; }
static const char *fname(const char *folder) { return folder && folder[0] ? folder : "(no folder)"; }

// "<prefix><s>" in capitals, for a title bar.
static void title_of(char *out, size_t n, const char *prefix, const char *s) {
    snprintf(out, n, "%s%s", prefix, s);
    for (char *p = out; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
}

static void count_text(char *out, size_t n, int count) {
    snprintf(out, n, "%d %s", count, count == 1 ? "backup" : "backups");
}

// ---------------------------------------------------------------------------
// The library and the device

static void reload() {
    n_ent = libsd::list(lu, info, ent, libsd::MAX_ENTRIES);
    int n_dirs = libsd::folder_dirs(lu, dirs, library::MAX_FOLDERS);
    n_fold = library::folders(ent, n_ent, dirs, n_dirs, fold, library::MAX_FOLDERS + 1);
}

static bool read_dev_crc() {
    char err[96];
    dev_crc_ok = jl::crc32(0, engine::FLASH_SIZE, &dev_crc, err, sizeof err);
    if (dev_crc_ok) ulog("The device holds CRC32 %08lX", (unsigned long)dev_crc);
    else ulog("Chip CRC failed: %s", err);
    return dev_crc_ok;
}

// The snapshot equal to what the device holds (its .txt's CRC32), or -1.
static int on_device() { return dev_crc_ok ? library::saved_as(ent, n_ent, dev_crc, nullptr, 0) : -1; }
static bool is_on_device(const library::Entry &e) {
    return dev_crc_ok && e.kind == library::SNAPSHOT && e.has_crc && e.crc == dev_crc;
}

// ---------------------------------------------------------------------------
// Keys, with the session kept busy

[[noreturn]] static void disconnected() {
    ulog("The device was disconnected.");
    begin("FIRMWARE MANAGER", FM);
    line(BODY_Y, 16, BOX, BAD, "Disconnected");
    para(BODY_Y + 22, 14, MSG, FG, "The device left USB. Connect it again from the home menu.");
    foot("ENTER home");
    end();
    for (;;) {
        int k = nav(ui::read_input());
        if (k == '\n' || k == K_BACK) modes::restart_into(modes::HOME);
        delay(15);
    }
}

// The session kept busy (a ping a second); a lost device ends it.
static void keep_busy() {
    static uint32_t last;
    if (millis() - last < 1000) return;
    last = millis();
    if (!engine::keepalive()) disconnected();
}

static int key() {
    for (;;) {
        int k = ui::read_input();
        if (k) return k;
        delay(15);
        keep_busy();
    }
}

// A message and ENTER (or ESC) to go on.
static void notice(const char *title, uint16_t barc, const char *big, uint16_t bigc, const char *l1,
                   const char *l2 = nullptr, uint16_t c2 = DIM) {
    begin(title, barc);
    line(BODY_Y, 16, BOX, bigc, big);
    int y = BODY_Y + 20;
    if (l1) y = para(y, 13, MSG, FG, l1, 3) + 3;
    if (l2) para(y, 12, TEXT, c2, l2, 3);
    foot("ENTER back");
    end();
    for (;;) {
        int k = nav(key());
        if (k == '\n' || k == K_BACK) return;
    }
}

// A screen while something runs: a big line, a bar (BUSY_BAR_Y), two notes.
static void busy(const char *title, uint16_t barc, const char *big, const char *note, uint16_t note_c = DIM,
                 const char *note2 = nullptr) {
    begin(title, barc);
    line(BODY_Y, 16, BOX, FG, big);
    lcd.drawRect(X, BUSY_BAR_Y, W - 2 * X, 10, DIM);
    if (note) line(BUSY_BAR_Y + 14, 12, TEXT, note_c, note);
    if (note2) line(BUSY_BAR_Y + 27, 12, TEXT, DIM, note2);
    foot("");
    end();
}

static void bar_progress(uint32_t done, uint32_t total) {
    progress(BUSY_BAR_Y, done, total);
    keep_busy();
}

// The save reads the device all the time: no ping needed.
static void read_progress(uint32_t done, uint32_t total) { progress(BUSY_BAR_Y, done, total); }

// A yes/no screen's keys: sel moved with the arrows; 0/1 on ENTER, -1 on ESC.
static int yesno_key(int *sel) {
    for (;;) {
        int k = nav(key());
        if (move(k, sel, 2, true)) return -2;  // redraw
        if (k == '\n') return *sel;
        if (k == K_BACK) return -1;
    }
}

// ---------------------------------------------------------------------------
// The name window (backup, rename, folders)

struct NameWin {
    const char *title;
    uint16_t color;
    const char *where;        // the line under the bar
    const char *list_folder;  // its names are listed (nullptr: none)
    // false: not taken (why says it, shown in the window); nullptr: any name
    bool (*accept)(const char *name, char *why, size_t n);
};

static bool name_window(const NameWin &w, char *buf, size_t cap) {
    char work[64], why[64] = "";
    if (cap > sizeof work) cap = sizeof work;
    put(work, cap, buf);
    int cnt = w.list_folder ? library::in_folder(ent, n_ent, w.list_folder, idx_buf, libsd::MAX_ENTRIES) : 0;
    Row *names = row_buf;  // the screens that call this draw their rows anew afterwards
    for (int i = 0; i < cnt; i++) {
        const library::Entry &e = ent[idx_buf[i]];
        names[i].ic = "";
        put(names[i].t, sizeof names[i].t, e.name);
        put(names[i].r, sizeof names[i].r, e.date);
        names[i].color = rgb(0xcc, 0xcc, 0xcc);
    }
    int pick = -1;  // the highlighted name, -1: none (the newest shown)
    bool cursor = true, redraw = true;
    uint32_t blink = millis();
    static constexpr int VIS = 4;
    for (;;) {
        if (redraw) {
            begin(w.title, w.color);
            line(BODY_Y, 11, SMALL, CY, w.where);
            input(BODY_Y + 13, work, cursor);
            int y = BODY_Y + 36;
            if (why[0]) {
                para(y, 11, TEXT, BAD, why, 2);
            } else if (!w.list_folder) {
                line(y, 11, SMALL, DIM, "Type the name.");
            } else if (!cnt) {
                line(y, 11, SMALL, DIM, "Nothing in this folder yet.");
            } else {
                line(y, 10, SMALL, DIM, "Already here (newest last):");
                int first = pick < 0 ? cnt - VIS : pick - 1;
                if (first > cnt - VIS) first = cnt - VIS;
                if (first < 0) first = 0;
                rows(y + 10, names, cnt, pick, VIS, 10, SMALL, first);
            }
            foot(cnt ? G_UP G_DOWN " names  TAB copy  ENTER save  ESC" : "type  BKSP delete  ENTER save  ESC");
            end();
            redraw = false;
        }
        int k = ui::read_input();
        size_t n = strlen(work);
        if (!k) {
            delay(15);
            keep_busy();
            if (millis() - blink > 500) {
                blink = millis();
                cursor = !cursor;
                lcd.startWrite();
                input(BODY_Y + 13, work, cursor);
                lcd.endWrite();
            }
            continue;
        }
        redraw = true;
        cursor = true;
        blink = millis();
        if (k == '\n') {
            char clean[64];
            library::clean_name(work, clean, sizeof clean);
            if (!clean[0]) continue;
            if (w.accept && !w.accept(work, why, sizeof why)) continue;
            put(buf, cap, work);
            return true;
        } else if (k == ui::IN_ESC) {
            return false;
        } else if (k == ui::IN_BKSP) {
            if (n) work[n - 1] = 0;
        } else if (k == ui::IN_DEL) {
            work[0] = 0;
        } else if (k == ui::IN_UP && cnt) {
            pick = pick < 0 ? cnt - 1 : pick > 0 ? pick - 1 : 0;
            continue;
        } else if (k == ui::IN_DOWN && cnt) {
            pick = pick < 0 ? cnt - 1 : pick + 1 < cnt ? pick + 1 : cnt - 1;
            continue;
        } else if (k == ui::IN_TAB && cnt) {
            put(work, cap, ent[idx_buf[pick < 0 ? cnt - 1 : pick]].name);
        } else if (k >= 0x20 && k < 0x7F) {
            if (n + 1 < cap) {
                work[n] = (char)k;
                work[n + 1] = 0;
            }
        } else {
            redraw = false;
            continue;
        }
        why[0] = 0;  // edited: the last refusal no longer applies
    }
}

// Folder names: made (or renamed) right in the window, so a refusal shows there.
static char folder_made[library::FOLDER_CHARS + 1];
static char folder_from[library::FOLDER_CHARS + 1];

static bool accept_new_folder(const char *name, char *why, size_t n) {
    return libsd::make_folder(&lu, info, name, folder_made, sizeof folder_made, why, n);
}
static bool accept_rename_folder(const char *name, char *why, size_t n) {
    return libsd::rename_folder(lu, folder_from, name, folder_made, sizeof folder_made, why, n);
}

// A new folder; true: folder_made is its name.
static bool new_folder(uint16_t color) {
    char name[library::FOLDER_CHARS + 1] = "";
    NameWin w = {"NEW FOLDER " G_MID " NAME", color, "A folder, e.g. the firmware's name", nullptr, accept_new_folder};
    if (!name_window(w, name, sizeof name)) return false;
    reload();
    return true;
}

// ---------------------------------------------------------------------------
// BACKUP CURRENT (and LOAD's "save first")

// Picks a folder ("+ New folder..." makes one). False: ESC.
static bool pick_folder(bool for_load, char *out, size_t outlen) {
    // the folder of the newest snapshot first
    int sel = 1, newest = -1;
    for (int f = 0; f < n_fold; f++)
        if (fold[f].newest >= 0 && (newest < 0 || ent[fold[f].newest].number > ent[newest].number)) {
            newest = fold[f].newest;
            sel = f + 1;
        }
    for (;;) {
        int n = 0;
        Row *r = row_buf;
        r[n].ic = "+";
        snprintf(r[n].t, sizeof r[n].t, "New folder...");
        r[n].r[0] = 0;
        r[n++].color = CY;
        for (int f = 0; f < n_fold; f++, n++) {
            r[n].ic = f ? G_TRI : "/";
            put(r[n].t, sizeof r[n].t, fname(fold[f].name));
            count_text(r[n].r, sizeof r[n].r, fold[f].count);
            r[n].color = 0;
        }
        if (sel >= n) sel = n - 1;
        begin(for_load ? "LOAD " G_MID " SAVE FIRST " G_MID " WHERE?" : "BACKUP CURRENT " G_MID " WHERE?", BACKUP);
        char path[80];
        if (sel == 0) snprintf(path, sizeof path, "Choose a folder for the backup");
        else if (fold[sel - 1].newest >= 0) snprintf(path, sizeof path, "Newest here: %s", ent[fold[sel - 1].newest].name);
        else snprintf(path, sizeof path, "Empty folder");
        line(BODY_Y, 11, SMALL, CY, path);
        rows(BODY_Y + 12, r, n, sel);
        foot(G_UP G_DOWN " move   ENTER choose   ESC back");
        end();
        int k = nav(key());
        if (move(k, &sel, n)) continue;
        if (k == K_BACK) return false;
        if (k != '\n') continue;
        if (sel == 0) {
            if (!new_folder(BACKUP)) continue;
            snprintf(out, outlen, "%s", folder_made);
            return true;
        }
        snprintf(out, outlen, "%s", fold[sel - 1].name);
        return true;
    }
}

enum SaveResult { SAVE_BACK, SAVE_DONE, SAVE_FAILED };

static SaveResult save_current(bool for_load) {
    char folder[library::FOLDER_CHARS + 1];
    for (;;) {
        if (!pick_folder(for_load, folder, sizeof folder)) return SAVE_BACK;
        char name[library::NAME_CHARS + 1], where[64];
        library::suggest(fw_hint, libsd::today(), unit.name, name, sizeof name);
        snprintf(where, sizeof where, "In: %s", fname(folder));
        NameWin w = {for_load ? "LOAD " G_MID " SAVE FIRST " G_MID " NAME" : "BACKUP CURRENT " G_MID " NAME", BACKUP,
                     where, folder, nullptr};
        if (!name_window(w, name, sizeof name)) continue;  // back to the folders

        busy(for_load ? "LOAD " G_MID " SAVING FIRST" : "BACKUP CURRENT " G_MID " SAVING", BACKUP,
             "Reading the device...", "1 MiB, about 4 s. Keep it connected.");
        int num;
        uint32_t crc;
        bool ok = libsd::save(&lu, info, folder, name, fw_hint, library::SRC_BACKUP, &num, &crc, read_progress);
        reload();
        if (!ok) {
            notice(for_load ? "LOAD " G_MID " SAVE FIRST" : "BACKUP CURRENT", BACKUP, "Not saved", BAD,
                   "Nothing was saved and nothing was written.", "INFO, then L: the log says why.");
            return SAVE_FAILED;
        }
        libsd::set_current(lu, num, name);
        dev_crc = crc;  // what the chip had, checked
        dev_crc_ok = true;
        if (for_load) return SAVE_DONE;

        // the same contents elsewhere: say so
        int same = -1;
        for (int i = 0; i < n_ent && same < 0; i++)
            if (ent[i].kind == library::SNAPSHOT && ent[i].number != num && ent[i].has_crc && ent[i].crc == crc) same = i;
        begin("BACKUP CURRENT " G_MID " DONE", BACKUP);
        line(BODY_Y, 16, BOX, GOOD, G_CHECK " Saved");
        char lb[80];
        snprintf(lb, sizeof lb, "%s%s%s", folder, folder[0] ? " / " : "", name);
        line(BODY_Y + 19, 13, MSG, FG, lb);
        line(BODY_Y + 34, 11, SMALL, DIM, "Checked against the device (CRC32).");
        if (same >= 0) {
            library::label(ent[same], lb, sizeof lb);
            line(BODY_Y + 50, 12, TEXT, WARN, "Same contents as an earlier backup:");
            line(BODY_Y + 62, 12, TEXT, WARN, lb);
        }
        foot("ENTER back to the menu");
        end();
        for (;;) {
            int k = nav(key());
            if (k == '\n' || k == K_BACK) return SAVE_DONE;
        }
    }
}

// ---------------------------------------------------------------------------
// LIBRARY

static bool confirm_delete(const char *what, const char *note, const char *warn) {
    int sel = 1;  // KEEP
    for (;;) {
        begin("LIBRARY " G_MID " DELETE", LIB);
        char q[80];
        snprintf(q, sizeof q, "Delete %s?", what);
        line(BODY_Y, 14, MSG, FG, q);
        line(BODY_Y + 15, 11, SMALL, DIM, note);
        if (warn) para(BODY_Y + 27, 12, TEXT, WARN, warn, 2);
        yesno(BODY_Y + 54, "DELETE", "KEEP", sel);
        foot(G_LEFT G_RIGHT " choose   ENTER   ESC keep");
        end();
        int c = yesno_key(&sel);
        if (c == -2) continue;
        return c == 0;
    }
}

// MOVE TO: the folder to move it to, or false.
static bool move_to(const library::Entry &e, char *out, size_t outlen) {
    int sel = 0;
    for (;;) {
        int n = 0;
        for (int f = 0; f < n_fold; f++, n++) {
            row_buf[n].ic = f ? G_TRI : "/";
            put(row_buf[n].t, sizeof row_buf[n].t, fname(fold[f].name));
            row_buf[n].r[0] = 0;
            row_buf[n].color = library::same_folder(fold[f].name, e.folder) ? DIM : 0;
        }
        begin("LIBRARY " G_MID " MOVE TO", LIB);
        char path[80];
        snprintf(path, sizeof path, "Move \"%s\" to:", e.name);
        line(BODY_Y, 11, SMALL, CY, path);
        rows(BODY_Y + 12, row_buf, n, sel);
        foot(G_UP G_DOWN "  ENTER move here  ESC cancel");
        end();
        int k = nav(key());
        if (move(k, &sel, n)) continue;
        if (k == K_BACK) return false;
        if (k == '\n') {
            snprintf(out, outlen, "%s", fold[sel].name);
            return true;
        }
    }
}

static void lib_items(const char *start_folder) {
    char folder[library::FOLDER_CHARS + 1];
    put(folder, sizeof folder, start_folder);
    int sel = 0;
    for (;;) {
        int cnt = library::in_folder(ent, n_ent, folder, idx_buf, libsd::MAX_ENTRIES);
        bool any_on = false;
        for (int i = 0; i < cnt; i++) {
            const library::Entry &e = ent[idx_buf[i]];
            row_buf[i].ic = is_on_device(e) ? G_DOT : "";
            any_on |= is_on_device(e);
            put(row_buf[i].t, sizeof row_buf[i].t, e.name);
            put(row_buf[i].r, sizeof row_buf[i].r, e.date);
            row_buf[i].color = e.has_crc ? 0 : DIM;
        }
        if (!cnt) {
            row_buf[0].ic = "";
            snprintf(row_buf[0].t, sizeof row_buf[0].t, "(empty)");
            row_buf[0].r[0] = 0;
            row_buf[0].color = DIM;
        }
        if (sel >= cnt) sel = cnt - 1;
        if (sel < 0) sel = 0;
        char title[64], path[64], c[24];
        title_of(title, sizeof title, "LIBRARY " G_MID " ", fname(folder));
        count_text(c, sizeof c, cnt);
        snprintf(path, sizeof path, "%s%s", c, any_on ? "  " G_MID " " G_DOT " = on the device now" : "");
        begin(title, LIB);
        line(BODY_Y, 11, SMALL, CY, path);
        rows(BODY_Y + 12, row_buf, cnt ? cnt : 1, cnt ? sel : -1);
        foot("N rename  M move  D delete  ESC");
        end();
        int k = nav(key());
        if (move(k, &sel, cnt)) continue;
        if (k == K_BACK) return;
        if (!cnt) continue;
        library::Entry e = ent[idx_buf[sel]];
        if (k == 'n') {
            char name[library::NAME_CHARS + 1], where[64];
            put(name, sizeof name, e.name);
            snprintf(where, sizeof where, "Was: %s", e.name);
            NameWin w = {"LIBRARY " G_MID " RENAME", LIB, where, folder, nullptr};
            if (!name_window(w, name, sizeof name)) continue;
            bool ok = libsd::rename(lu, e, name);
            reload();
            if (!ok) notice("LIBRARY " G_MID " RENAME", LIB, "Not renamed", BAD, "The SD card refused it.", "INFO, then L: the log says why.");
        } else if (k == 'm') {
            char to[library::FOLDER_CHARS + 1];
            if (!move_to(e, to, sizeof to)) continue;
            bool ok = libsd::move(lu, e, to);
            reload();
            if (!ok) {
                notice("LIBRARY " G_MID " MOVE TO", LIB, "Not moved", BAD, "A file of that name is there already, or the SD card refused it.");
                continue;
            }
            // follow it into its new folder
            put(folder, sizeof folder, to);
            cnt = library::in_folder(ent, n_ent, folder, idx_buf, libsd::MAX_ENTRIES);
            for (int i = 0; i < cnt; i++)
                if (ent[idx_buf[i]].number == e.number) sel = i;
        } else if (k == 'd') {
            bool on = is_on_device(e), copy = false;
            for (int i = 0; i < n_ent; i++)
                copy |= ent[i].kind == library::SNAPSHOT && ent[i].number != e.number && is_on_device(ent[i]);
            const char *warn = on && !copy ? "It is what the device holds now: afterwards that is not saved." :
                               library::count_snapshots(ent, n_ent) == 1 ? "It is this device's only backup." : nullptr;
            if (!confirm_delete(e.name, "The file is removed from the SD card.", warn)) continue;
            bool ok = libsd::remove(lu, e);
            reload();
            if (!ok) notice("LIBRARY " G_MID " DELETE", LIB, "Not deleted", BAD, "The SD card refused it.");
        }
    }
}

static void check_progress(uint32_t done, uint32_t total) { bar_progress(done, total); }

static void import_old(const library::Entry &e) {
    char big[48], err[128];
    snprintf(big, sizeof big, "Checking %s", e.name);
    busy("LIBRARY " G_MID " IMPORT", LIB, big, "Read twice and compared with the device.");
    static writer::Source src;
    static library::Meta meta;
    char path[64];
    uint32_t size = 0;
    libsd::bin_path(lu, e, path, sizeof path);
    bool has_meta = libsd::read_meta(lu, e, &meta);
    if (!engine::open_file(path, &src, &size)) {
        notice("LIBRARY " G_MID " IMPORT", LIB, "Cannot open it", BAD, path);
        return;
    }
    restore::Result r;
    bool read = restore::check(info, is_smk37(), size, has_meta ? &meta.notes : nullptr, src, check_progress, &r, err,
                               sizeof err);
    engine::close_backup();
    if (!read) {
        notice("LIBRARY " G_MID " IMPORT", LIB, "Could not read", BAD, err, "Nothing imported.");
        return;
    }
    ulog("%s: %s", e.name, r.why);
    if (!restore::allowed(r.verdict) || !library::same_model(unit.name, meta.model)) {
        notice("LIBRARY " G_MID " IMPORT", LIB, "Not imported", BAD,
               r.verdict == restore::BAD_FILE ? "Not a whole, intact backup." : "It is not this device's backup.",
               r.why);
        return;
    }
    // I takes the OLD FILE, not the device as it is now (that is BACKUP CURRENT)
    int same = e.has_crc ? library::find_crc(ent, n_ent, e.crc) : -1;
    int sel = 0;
    for (;;) {
        begin("LIBRARY " G_MID " IMPORT", LIB);
        char s[128];
        snprintf(s, sizeof s, "%s holds CRC32 %08lX", e.name, (unsigned long)e.crc);
        line(BODY_Y, 13, MSG, FG, s);
        if (same >= 0) {
            char lb[80];
            library::label(ent[same], lb, sizeof lb);
            snprintf(s, sizeof s, "Already saved: %s", lb);
        } else {
            snprintf(s, sizeof s, "Not in the library yet.");
        }
        line(BODY_Y + 14, 12, TEXT, same >= 0 ? WARN : FG, s);
        bool now = dev_crc_ok && e.crc == dev_crc;
        line(BODY_Y + 26, 12, TEXT, now ? GOOD : WARN, now ? "= what the device holds now" : "NOT what the device holds now");
        line(BODY_Y + 40, 10, SMALL, DIM, "This imports the old file, not the");
        line(BODY_Y + 50, 10, SMALL, DIM, "device now (that is BACKUP CURRENT).");
        yesno(BODY_Y + 64, "IMPORT", "CANCEL", sel);
        foot(G_LEFT G_RIGHT " choose   ENTER   ESC cancel");
        end();
        int c = yesno_key(&sel);
        if (c == -2) continue;
        if (c != 0) return;
        break;
    }
    char name[library::NAME_CHARS + 1];
    put(name, sizeof name, e.name);
    NameWin w = {"LIBRARY " G_MID " IMPORT " G_MID " NAME", LIB, "Into: (no folder). Name it after what it holds.", "",
                 nullptr};
    if (!name_window(w, name, sizeof name)) return;
    busy("LIBRARY " G_MID " IMPORTING", LIB, "Copying the old backup...", "Checked against its CRC32.");
    int num;
    bool ok = libsd::import(&lu, info, e, name, &num, bar_progress);
    reload();
    if (ok) notice("LIBRARY " G_MID " IMPORT", LIB, G_CHECK " Imported", GOOD, name, "In \"(no folder)\": M moves it.");
    else notice("LIBRARY " G_MID " IMPORT", LIB, "Not imported", BAD, "INFO, then L: the log says why.");
}

static void old_backups() {
    int sel = 0;
    for (;;) {
        int n = 0;
        for (int i = 0; i < n_ent; i++) {
            const library::Entry &e = ent[i];
            if (e.kind != library::LEGACY) continue;
            idx_buf[n] = i;
            row_buf[n].ic = "";
            put(row_buf[n].t, sizeof row_buf[n].t, e.name);
            put(row_buf[n].r, sizeof row_buf[n].r, library::whose_name(e.whose));
            row_buf[n].color = e.whose == library::WHOSE_OTHER ? BAD : e.whose == library::WHOSE_THIS ? 0 : DIM;
            n++;
        }
        if (!n) return;
        if (sel >= n) sel = n - 1;
        begin("LIBRARY " G_MID " OLD BACKUPS", LIB);
        line(BODY_Y, 11, SMALL, CY, "Rescue backups (backup-NNN), read-only");
        rows(BODY_Y + 12, row_buf, n, sel);
        foot("I import   ESC folders");
        end();
        int k = nav(key());
        if (move(k, &sel, n)) continue;
        if (k == K_BACK) return;
        if (k == 'i') {
            library::Entry e = ent[idx_buf[sel]];
            import_old(e);
        }
    }
}

static void library_menu() {
    int sel = 0;
    for (;;) {
        reload();
        int n = 0, n_old = 0;
        for (int i = 0; i < n_ent; i++) n_old += ent[i].kind == library::LEGACY;
        for (int f = 0; f < n_fold; f++, n++) {
            row_buf[n].ic = f ? G_TRI : "/";
            put(row_buf[n].t, sizeof row_buf[n].t, fname(fold[f].name));
            snprintf(row_buf[n].r, sizeof row_buf[n].r, "%d", fold[f].count);
            row_buf[n].color = 0;
        }
        int old_row = -1;
        if (n_old) {
            old_row = n;
            row_buf[n].ic = G_TRI;
            snprintf(row_buf[n].t, sizeof row_buf[n].t, "Old backups");
            snprintf(row_buf[n].r, sizeof row_buf[n].r, "%d", n_old);
            row_buf[n++].color = DIM;
        }
        if (sel >= n) sel = n - 1;
        char title[48];
        title_of(title, sizeof title, "LIBRARY " G_MID " ", unit.name);
        begin(title, LIB);
        line(BODY_Y, 11, SMALL, HOME, "F new folder  N rename  D delete");
        rows(BODY_Y + 12, row_buf, n, sel);
        foot("ENTER open folder   ESC back");
        end();
        int k = nav(key());
        if (move(k, &sel, n)) continue;
        if (k == K_BACK) return;
        bool real = sel > 0 && sel != old_row;  // a folder of the user's
        if (k == '\n') {
            if (sel == old_row) old_backups();
            else lib_items(fold[sel].name);
        } else if (k == 'f') {
            if (new_folder(LIB))
                for (int f = 0; f < n_fold; f++)
                    if (!strcmp(fold[f].name, folder_made)) sel = f;
        } else if (k == 'n') {
            if (!real) {
                notice("LIBRARY", LIB, "Not this one", WARN, "\"(no folder)\" and \"Old backups\" keep their names.");
                continue;
            }
            put(folder_from, sizeof folder_from, fold[sel].name);
            char name[library::FOLDER_CHARS + 1], where[64];
            put(name, sizeof name, fold[sel].name);
            snprintf(where, sizeof where, "Was: %s", fold[sel].name);
            NameWin w = {"LIBRARY " G_MID " RENAME FOLDER", LIB, where, nullptr, accept_rename_folder};
            if (name_window(w, name, sizeof name)) {
                reload();
                for (int f = 0; f < n_fold; f++)
                    if (!strcmp(fold[f].name, folder_made)) sel = f;
            }
        } else if (k == 'd') {
            if (!real) {
                notice("LIBRARY", LIB, "Not this one", WARN, "\"(no folder)\" and \"Old backups\" cannot be deleted.");
                continue;
            }
            if (fold[sel].count) {
                char c[24], s[96];
                count_text(c, sizeof c, fold[sel].count);
                snprintf(s, sizeof s, "%s in it. Move or delete them first (open it: M, D).", c);
                notice("LIBRARY " G_MID " DELETE", LIB, "Not empty", WARN, s);
                continue;
            }
            char what[48], why[64];
            snprintf(what, sizeof what, "the folder %s", fold[sel].name);
            if (!confirm_delete(what, "It is empty.", nullptr)) continue;
            if (!libsd::remove_folder(lu, fold[sel].name, why, sizeof why))
                notice("LIBRARY " G_MID " DELETE", LIB, "Not deleted", BAD, why);
        }
    }
}

// ---------------------------------------------------------------------------
// LOAD FIRMWARE

[[noreturn]] static void exit_device();

enum Phase { CHECKING, WRITING };
static Phase phase;

static bool battery_hook(void *, char *why, size_t n) {
    int mv = M5.Power.getBatteryVoltage(), pct;
    library::Battery b = library::battery(mv, &pct);
    if (b == library::BATTERY_UNKNOWN) {
        ulog("Battery: no believable reading (%d mV): not checked", mv);
        return true;
    }
    ulog("Cardputer battery %d %% (%d mV)", pct, mv);
    if (b == library::BATTERY_OK) return true;
    snprintf(why, n, "The Cardputer's battery is at %d %%, under %d %%: charge it first.", pct,
             library::BATTERY_MIN_PERCENT);
    return false;
}

static void load_progress(uint32_t done, uint32_t total) { bar_progress(done, total); }

struct LoadCtx {
    const library::Entry *e;
};

static void writing_screen(const library::Entry &e) {
    char big[64];
    snprintf(big, sizeof big, "Writing %s", e.name);
    busy("LOAD FIRMWARE " G_MID " WRITING", LOAD, big, "Do not switch anything off.", WARN,
         "Each piece is checked after writing.");
}

// Seconds a write may take: an estimate (erasing and writing a sector, and
// the checks at the end).
static uint32_t estimate_s(const writer::Plan &p) { return 3 + (p.to_write * 90 + p.to_erase * 45) / 1000; }

static switcher::Choice confirm_hook(void *ctx, int stage, const switcher::Report &r) {
    const library::Entry &e = *((LoadCtx *)ctx)->e;
    char s[96], lb[80];
    library::label(e, lb, sizeof lb);
    if (stage == 0) {
        int sel = 0;
        for (;;) {
            begin("LOAD FIRMWARE " G_MID " CONFIRM", LOAD);
            snprintf(s, sizeof s, "Load %s", lb);
            line(BODY_Y, 13, MSG, FG, s);
            kv(BODY_Y + 14, "Checks", "this device's " G_CHECK "  intact " G_CHECK, GOOD);
            snprintf(s, sizeof s, "%lu sectors " G_MID " ~%lu s", (unsigned long)(r.plan.to_write + r.plan.to_erase),
                     (unsigned long)estimate_s(r.plan));
            kv(BODY_Y + 26, "Changes", s);
            kv(BODY_Y + 38, "Kept", is_smk37() ? "startup, calibration" : "startup");
            int y = BODY_Y + 54;
            if (r.boot_differs) {
                kv(BODY_Y + 50, "Startup", "differs here (X: write it too)", WARN);
                y = BODY_Y + 66;
            }
            yesno(y, "LOAD", "CANCEL", sel);
            foot(r.boot_differs ? G_LEFT G_RIGHT "  ENTER  X exactly  ESC cancel" : G_LEFT G_RIGHT " choose   ENTER   ESC cancel");
            end();
            int k = nav(key());
            if (move(k, &sel, 2, true)) continue;
            if (k == K_BACK) return switcher::CANCEL;
            if (k == 'x' && r.boot_differs) {
                busy("LOAD FIRMWARE " G_MID " CHECKING", LOAD, "Comparing it all", "The startup area too.");
                return switcher::EXACT;
            }
            if (k == '\n') {
                if (sel != 0) return switcher::CANCEL;
                phase = WRITING;
                writing_screen(e);
                return switcher::WRITE;
            }
        }
    }
    int sel = 1;  // CANCEL
    for (;;) {
        begin("LOAD FIRMWARE " G_MID " EXACTLY?", BAD);
        line(BODY_Y, 13, MSG, FG, "This writes the startup area too");
        line(BODY_Y + 13, 13, MSG, FG, "(0x0-0x3FFF): the riskiest write.");
        para(BODY_Y + 28, 11, TEXT, DIM,
             "It goes last; if it stops there the device may need Recovery. Only if this firmware needs its own.", 3);
        snprintf(s, sizeof s, "%lu sectors " G_MID " ~%lu s", (unsigned long)(r.plan.to_write + r.plan.to_erase),
                 (unsigned long)estimate_s(r.plan));
        kv(BODY_Y + 62, "Changes", s);
        yesno(BODY_Y + 77, "WRITE ALL", "CANCEL", sel);
        foot(G_LEFT G_RIGHT " choose   ENTER   ESC cancel");
        end();
        int c = yesno_key(&sel);
        if (c == -2) continue;
        if (c != 0) return switcher::CANCEL;
        phase = WRITING;
        writing_screen(e);
        return switcher::WRITE;
    }
}

static void load_same(const library::Entry &e) {
    begin("LOAD FIRMWARE", LOAD);
    line(BODY_Y, 16, BOX, GOOD, "Already on the device");
    char s[80];
    snprintf(s, sizeof s, "%s is what it holds now.", e.name);
    para(BODY_Y + 20, 13, MSG, FG, s, 2);
    line(BODY_Y + 48, 13, MSG, FG, "Nothing to write.");
    foot("ENTER / ESC back");
    end();
    for (;;) {
        int k = nav(key());
        if (k == '\n' || k == K_BACK) return;
    }
}

// True: back to the main menu (loaded, or a write that stopped).
static bool do_load(const library::Entry &e) {
    char big[64], path[128], err[128];
    snprintf(big, sizeof big, "Checking %s", e.name);
    busy("LOAD FIRMWARE " G_MID " CHECKING", LOAD, big, "Read twice, compared with the device.");
    phase = CHECKING;
    static writer::Source src;
    static library::Meta meta;
    uint32_t size = 0;
    libsd::bin_path(lu, e, path, sizeof path);
    bool has_meta = libsd::read_meta(lu, e, &meta);
    if (!engine::open_file(path, &src, &size)) {
        notice("LOAD FIRMWARE", LOAD, "Cannot open it", BAD, path, "Nothing written.");
        return false;
    }
    LoadCtx ctx = {&e};
    switcher::Request req = {};
    req.info = &info;
    req.smk37 = is_smk37();
    req.unit_model = unit.name;
    req.snap_model = has_meta ? meta.model : "";
    req.size = size;
    req.notes = has_meta ? &meta.notes : nullptr;
    req.src = &src;
    req.name = e.base;
    switcher::Hooks h = {};
    h.ctx = &ctx;
    h.battery = battery_hook;
    h.confirm = confirm_hook;
    h.log = ulog;
    h.progress = load_progress;
    static switcher::Report rep;
    uint32_t t0 = millis();
    switcher::Outcome o = switcher::load(req, h, &rep, err, sizeof err);
    engine::close_backup();
    ulog("Load: %s", switcher::outcome_name(o));
    switch (o) {
        case switcher::WRITTEN: {
            ulog("Done in %lu s.", (unsigned long)((millis() - t0) / 1000));
            libsd::set_current(lu, e.number, e.name);
            read_dev_crc();
            reload();
            int sel = 0;
            static const Box b[] = {{"RESTART DEVICE", "Start it with this firmware"}, {"BACK TO MENU", nullptr}};
            for (;;) {
                begin("LOAD FIRMWARE " G_MID " DONE", LOAD);
                line(BODY_Y, 16, BOX, GOOD, G_CHECK " Loaded and verified");
                line(BODY_Y + 18, 13, MSG, FG, e.name);
                boxes(BODY_Y + 33, b, 2, sel, 27);
                foot(G_UP G_DOWN " ENTER");
                end();
                int k = nav(key());
                if (move(k, &sel, 2)) continue;
                if (k == '1' || k == '2') {
                    sel = k - '1';
                    k = '\n';
                }
                if (k == '\n' && sel == 0) exit_device();
                if (k == '\n' || k == K_BACK) return true;
            }
        }
        case switcher::NOTHING_TO_DO:
            libsd::set_current(lu, e.number, e.name);
            load_same(e);
            return false;
        case switcher::CANCELLED:
            return false;
        case switcher::WRITE_FAILED:
            read_dev_crc();
            reload();
            notice("LOAD FIRMWARE", LOAD, "Load stopped", BAD, err,
                   "Do NOT switch the device off. LOAD it again to finish.", WARN);
            return true;
        case switcher::REFUSED:
            notice("LOAD FIRMWARE", LOAD, "Refused", BAD, err, "Nothing written.");
            return false;
        case switcher::BATTERY_LOW:
            notice("LOAD FIRMWARE", LOAD, "Battery low", BAD, err, "Nothing written.");
            return false;
        default:
            notice("LOAD FIRMWARE", LOAD, "Could not read", BAD, err, "Nothing written.");
            return false;
    }
}

// True: back to the main menu.
static bool load_items(const char *folder) {
    int sel = 0;
    for (;;) {
        int cnt = library::in_folder(ent, n_ent, folder, idx_buf, libsd::MAX_ENTRIES);
        for (int i = 0; i < cnt; i++) {
            const library::Entry &e = ent[idx_buf[i]];
            row_buf[i].ic = is_on_device(e) ? G_DOT : "";
            put(row_buf[i].t, sizeof row_buf[i].t, e.name);
            put(row_buf[i].r, sizeof row_buf[i].r, e.date);
            row_buf[i].color = e.has_crc ? 0 : DIM;
        }
        if (!cnt) {
            row_buf[0].ic = "";
            snprintf(row_buf[0].t, sizeof row_buf[0].t, "(empty)");
            row_buf[0].r[0] = 0;
            row_buf[0].color = DIM;
        }
        if (sel >= cnt) sel = cnt ? cnt - 1 : 0;
        char title[64];
        title_of(title, sizeof title, "LOAD " G_MID " ", fname(folder));
        begin(title, LOAD);
        line(BODY_Y, 11, SMALL, CY, "Which one goes on the device?");
        rows(BODY_Y + 12, row_buf, cnt ? cnt : 1, cnt ? sel : -1);
        foot(G_UP G_DOWN " move   ENTER choose   ESC folders");
        end();
        int k = nav(key());
        if (move(k, &sel, cnt)) continue;
        if (k == K_BACK) return false;
        if (k != '\n' || !cnt) continue;
        library::Entry e = ent[idx_buf[sel]];
        if (is_on_device(e)) {
            load_same(e);
            continue;
        }
        if (!e.has_crc) {
            notice("LOAD FIRMWARE", LOAD, "Cannot be loaded", BAD, "It has no .txt with its CRC32, so it cannot be checked.");
            continue;
        }
        if (do_load(e)) return true;
        reload();
    }
}

static void load_choose() {
    int sel = 0;
    for (;;) {
        // the folders, then "(no folder)"
        int n = 0, order[library::MAX_FOLDERS + 1];
        for (int f = 1; f < n_fold; f++) order[n++] = f;
        order[n++] = 0;
        for (int i = 0; i < n; i++) {
            const library::Folder &f = fold[order[i]];
            row_buf[i].ic = order[i] ? G_TRI : "/";
            put(row_buf[i].t, sizeof row_buf[i].t, fname(f.name));
            snprintf(row_buf[i].r, sizeof row_buf[i].r, "%d", f.count);
            row_buf[i].color = f.count ? 0 : DIM;
        }
        if (sel >= n) sel = n - 1;
        begin("LOAD FIRMWARE " G_MID " CHOOSE", LOAD);
        int on = on_device();
        char path[96] = "Pick a folder";
        if (on >= 0) {
            char lb[80];
            library::label(ent[on], lb, sizeof lb);
            snprintf(path, sizeof path, "On device: %s " G_CHECK, lb);
        }
        line(BODY_Y, 11, SMALL, CY, path);
        rows(BODY_Y + 12, row_buf, n, sel);
        foot("ENTER open   ESC back");
        end();
        int k = nav(key());
        if (move(k, &sel, n)) continue;
        if (k == K_BACK) return;
        if (k == '\n') {
            char folder[library::FOLDER_CHARS + 1];
            put(folder, sizeof folder, fold[order[sel]].name);
            if (load_items(folder)) return;
        }
    }
}

static void load_firmware() {
    busy("LOAD FIRMWARE", LOAD, "Checking the library...", "Is what the device holds saved?");
    read_dev_crc();
    reload();
    // saved (any folder, its file checked): no question
    if (dev_crc_ok && libsd::saved_as(lu, ent, n_ent, dev_crc) >= 0) {
        load_choose();
        return;
    }
    int sel = 0;
    for (;;) {
        begin("LOAD FIRMWARE " G_MID " SAVE FIRST?", LOAD);
        line(BODY_Y, 13, MSG, FG, "What is on the device now is");
        int x = text(X, BODY_Y + 13 + 6, "not saved", MSG, WARN);
        text(X + 1, BODY_Y + 13 + 6, "not saved", MSG, WARN);  // bold
        text(x + 1, BODY_Y + 13 + 6, " in the library.", MSG, FG);
        line(BODY_Y + 28, 11, SMALL, FG, "Save it before loading another?");
        yesno(BODY_Y + 42, "YES, SAVE IT", "NO", sel);
        line(BODY_Y + 70, 12, TEXT, DIM, "No: unsaved patterns on the device are lost.");
        foot(G_LEFT G_RIGHT " choose   ENTER   ESC back");
        end();
        int c = yesno_key(&sel);
        if (c == -2) continue;
        if (c < 0) return;
        if (c == 1) {
            ulog("LOAD: what the device holds is not saved; not saved first (NO).");
            break;
        }
        SaveResult s = save_current(true);
        if (s == SAVE_BACK) continue;
        if (s == SAVE_FAILED) return;
        break;
    }
    load_choose();
}

// ---------------------------------------------------------------------------
// INFO, the log, exit

static void log_view() {
    static constexpr int COLS = 39, MAX_LINES = 200, VIS = 11;
    static constexpr size_t BUF = 6001;
    // only while the log is shown: not kept in RAM the rest of the time
    char (*lines)[COLS + 1] = (char (*)[COLS + 1])malloc(MAX_LINES * (COLS + 1));
    char *buf = (char *)malloc(BUF);
    int n = 0;
    if (!lines || !buf) {
        free(lines);
        free(buf);
        notice("INFO " G_MID " LOG", INFO, "No memory", BAD, "The log cannot be shown now.");
        return;
    }
    if (sd_ready()) {
        File f = SD.open("/fm1/log.txt", FILE_READ);
        if (f) {
            size_t size = f.size(), from = size > BUF - 1 ? size - (BUF - 1) : 0;
            f.seek(from);
            int got = f.read((uint8_t *)buf, BUF - 1);
            f.close();
            buf[got > 0 ? got : 0] = 0;
            char *p = buf;
            if (from) {  // the first line is cut: skip it
                char *nl = strchr(p, '\n');
                p = nl ? nl + 1 : p + strlen(p);
            }
            while (*p) {
                char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                const char *s = p;
                while (len && *s == ' ') {
                    s++;
                    len--;
                }
                for (size_t off = 0; off < len || off == 0; off += COLS) {
                    if (n == MAX_LINES) {  // keep the newest
                        memmove(lines[0], lines[1], (COLS + 1) * (MAX_LINES - 1));
                        n--;
                    }
                    size_t k = len - off < COLS ? len - off : COLS;
                    memcpy(lines[n], s + off, k);
                    lines[n][k] = 0;
                    for (size_t c = 0; c < k; c++)
                        if ((unsigned char)lines[n][c] < 0x20 || (unsigned char)lines[n][c] > 0x7E) lines[n][c] = ' ';
                    n++;
                    if (len <= off + COLS) break;
                }
                p = nl ? nl + 1 : p + strlen(p);
            }
        }
    }
    int top = n > VIS ? n - VIS : 0;
    for (;;) {
        begin("INFO " G_MID " LOG", INFO);
        if (!n) line(BODY_Y, 11, SMALL, DIM, sd_ok ? "The log is empty." : "No SD card: no log.");
        for (int i = 0; i < VIS && top + i < n; i++) text(X - 2, BODY_Y + i * 9 + 4, lines[top + i], SMALL, rgb(0xcc, 0xcc, 0xcc));
        foot(G_UP G_DOWN " scroll   ESC back");
        end();
        int k = nav(key());
        if (k == K_UP && top > 0) top--;
        else if (k == K_DOWN && top + VIS < n) top++;
        else if (k == K_LEFT) top = top > VIS ? top - VIS : 0;
        else if (k == K_RIGHT) top = top + 2 * VIS < n ? top + VIS : (n > VIS ? n - VIS : 0);
        else if (k == K_BACK || k == '\n') break;
    }
    free(lines);
    free(buf);
}

static void info_screen() {
    char sd[48] = "no card";
    if (sd_ready()) {
        uint64_t total = SD.totalBytes(), used = SD.usedBytes(), free_b = total > used ? total - used : 0;
        char c[24];
        count_text(c, sizeof c, library::count_snapshots(ent, n_ent));
        if (free_b >= 1000000000ULL)
            snprintf(sd, sizeof sd, "%lu.%lu GB free " G_MID " %s", (unsigned long)(free_b / 1000000000ULL),
                     (unsigned long)(free_b % 1000000000ULL / 100000000ULL), c);
        else snprintf(sd, sizeof sd, "%lu MB free " G_MID " %s", (unsigned long)(free_b / 1000000ULL), c);
    }
    for (;;) {
        begin("INFO", INFO);
        int on = on_device();
        char lb[80], bat[16];
        if (on >= 0) library::label(ent[on], lb, sizeof lb);
        else snprintf(lb, sizeof lb, "not saved");
        int pct = battery_percent();
        if (pct >= 0) snprintf(bat, sizeof bat, "%d %%", pct);
        else snprintf(bat, sizeof bat, "no reading");
        kv(BODY_Y, "Device", unit.name);
        kv(BODY_Y + 12, "Firmware", fw_hint[0] ? fw_hint : "(not known)", fw_hint[0] ? FG : DIM);
        kv(BODY_Y + 24, "On device", lb, on >= 0 ? GOOD : WARN);
        kv(BODY_Y + 36, "Unit ID", lu.id);
        kv(BODY_Y + 48, "Battery", bat);
        kv(BODY_Y + 60, "SD card", sd);
        kv(BODY_Y + 72, "App", APP_VERSION, DIM);
        foot("L view log   ESC back");
        end();
        int k = nav(key());
        if (k == 'l') log_view();
        else if (k == K_BACK || k == '\n') return;
    }
}

[[noreturn]] static void exit_device() {
    char err[96];
    bool ok = jl::restart(err, sizeof err);
    ulog(ok ? "Restarted: the device boots its firmware." : "Restart failed");
    begin("FIRMWARE MANAGER", FM);
    line(BODY_Y, 16, BOX, ok ? GOOD : BAD, ok ? "Device restarted" : "Restart failed");
    line(BODY_Y + 20, 13, MSG, FG, ok ? "You can unplug it." : "Switch it off and on again.");
    foot("ENTER home");
    end();
    ui::wait_released();
    for (;;) {
        int k = nav(ui::read_input());
        if (k == '\n' || k == K_BACK) modes::restart_into(modes::HOME);
        delay(15);
    }
}

static void exit_ask() {
    static const Box b[] = {{"RESTART DEVICE", "It starts its firmware; you can play"}, {"STAY", nullptr}};
    int sel = 0;
    for (;;) {
        begin("FIRMWARE MANAGER " G_MID " EXIT", FM);
        boxes(BODY_Y, b, 2, sel, 27);
        foot(G_UP G_DOWN " ENTER   ESC stay");
        end();
        int k = nav(key());
        if (move(k, &sel, 2)) continue;
        if (k == '1' || k == '2') {
            sel = k - '1';
            k = '\n';
        }
        if (k == '\n' && sel == 0) exit_device();
        if (k == '\n' || k == K_BACK) return;
    }
}

// ---------------------------------------------------------------------------
// The main menu

static void main_menu() {
    static const Box b[] = {{"BACKUP CURRENT", nullptr}, {"LIBRARY", nullptr}, {"LOAD FIRMWARE", nullptr}};
    int sel = 0;
    for (;;) {
        char title[48];
        title_of(title, sizeof title, "FIRMWARE MANAGER " G_MID " ", unit.name);
        begin(title, FM);
        int on = on_device();
        int x = text(X, BODY_Y + 5, "On device: ", SMALL, FG);
        if (on >= 0) {
            char lb[80], s[96];
            library::label(ent[on], lb, sizeof lb);
            snprintf(s, sizeof s, "%s " G_CHECK " saved", lb);
            text(x, BODY_Y + 5, s, SMALL, GOOD, W - X - x);
        } else {
            text(x, BODY_Y + 5, dev_crc_ok ? "not saved in the library" : "not known", SMALL, WARN, W - X - x);
        }
        boxes(BODY_Y + 13, b, 3, sel, 27);
        foot(G_UP G_DOWN " ENTER  I info  X exit, restart it");
        end();
        int k = nav(key());
        if (move(k, &sel, 3)) continue;
        if (k >= '1' && k <= '3') {
            sel = k - '1';
            k = '\n';
        }
        if (k == '\n') {
            if (sel == 0) save_current(false);
            else if (sel == 1) library_menu();
            else load_firmware();
            reload();
        } else if (k == 'i') {
            info_screen();
        } else if (k == 'x') {
            exit_device();
        } else if (k == K_BACK) {
            exit_ask();
        }
    }
}

// ---------------------------------------------------------------------------
// Connect

static bool connect_screen() {
    bool no_card = false;
    ui::wait_released();
    for (;;) {
        begin("FIRMWARE MANAGER " G_MID " CONNECT", FM);
        line(BODY_Y, 16, BOX, FG, "Connect the device");
        line(BODY_Y + 19, 13, MSG, FG, "SMK-37 or FM-1 to the Cardputer");
        line(BODY_Y + 32, 13, MSG, FG, "with a USB-C cable.");
        line(BODY_Y + 48, 12, TEXT, DIM, "On or off: both work.");
        line(BODY_Y + 60, 12, TEXT, DIM, "Cardputer on battery.");
        if (no_card) line(BODY_Y + 78, 12, TEXT, BAD, "No SD card: insert one (FAT32).");
        foot("ENTER continue   ESC back");
        end();
        int k = 0;
        while (!(k = nav(ui::read_input()))) delay(15);
        if (k == K_BACK) return false;
        if (k == '\n') {
            if (sd_ready()) return true;
            no_card = true;
        }
    }
}

static int wait_step;  // 0 waiting, 1 found, 2 asking, 3 connected
static char found_line[80], ask_line[48], done_line[48];
static uint32_t ask_t0;

static void draw_wait(const char *big, const char *note, uint16_t note_c, const char *footer) {
    begin("FIRMWARE MANAGER " G_MID " CONNECTING", FM, false);  // no room for the battery
    line(BODY_Y, 16, BOX, FG, big);
    const char *l[3] = {found_line[0] ? found_line : "Found: ...", ask_line, done_line};
    for (int i = 0; i < 3; i++) {
        char s[96];
        bool done = wait_step >= i + 1;
        snprintf(s, sizeof s, "%s%s", done ? G_CHECK " " : G_MID " ", l[i]);
        line(BODY_Y + 19 + i * 13, 13, TEXT, done ? GOOD : DIM, s);
    }
    para(BODY_Y + 61, 11, TEXT, note_c, note, 2);
    foot(footer);
    end();
}

// While waiting (the USB host, the soft key): ESC cancels; the seconds of
// the MIDI request are shown.
static bool wait_tick() {
    static uint32_t last;
    if (wait_step == 2 && millis() - last > 1000) {
        last = millis();
        char s[16];
        snprintf(s, sizeof s, "%lus", (unsigned long)((millis() - ask_t0) / 1000));
        lcd.startWrite();
        lcd.fillRect(W - 30, BODY_Y + 19 + 13, 30 - X, 13, BG);
        text(W - X - width(s, SMALL), BODY_Y + 19 + 13 + 6, s, SMALL, DIM);
        lcd.endWrite();
    }
    int k = ui::read_input();
    return k == ui::IN_ESC || k == ui::IN_BKSP || k == ui::IN_DEL;
}

[[noreturn]] static void connect_failed(const char *big, const char *note) {
    draw_wait(big, note, BAD, "ENTER try again   ESC home");
    ui::wait_released();
    for (;;) {
        int k = nav(ui::read_input());
        if (k == '\n') modes::restart_into(modes::FW_MANAGER);  // the USB host is up: start clean
        if (k == K_BACK) modes::restart_into(modes::HOME);
        delay(15);
    }
}

static void connecting() {
    wait_step = 0;
    found_line[0] = 0;
    snprintf(ask_line, sizeof ask_line, "Asking it for download mode...");
    snprintf(done_line, sizeof done_line, "Connected");
    static const char *HINT = "Not answering over MIDI? Use Recovery.";
    draw_wait("Switch the device ON", HINT, DIM, "ESC cancel");
    ui::wait_released();
    ulog("Firmware manager %s: connect, no power-on key", APP_VERSION);
    char err[96];
    fw_hint[0] = 0;
    engine::ConnectResult c = engine::connect_any(&info, DEVICE_WAIT_MS, wait_tick, err, sizeof err);
    if (c == engine::CONN_CANCELLED) modes::restart_into(modes::FW_MANAGER);
    if (c == engine::CONN_NOT_ROM) {
        const engine::Other &o = engine::other;
        snprintf(found_line, sizeof found_line, "Found: %s%s", o.product[0] ? o.product : "a USB device",
                 o.midi == usbmidi::FOUND ? " (MIDI)" : "");
        wait_step = 1;
        if (o.midi != usbmidi::FOUND) {
            ulog("Not USB MIDI: the firmware manager cannot ask it.");
            connect_failed("Not USB MIDI", "Not answering over MIDI. Use Recovery.");
        }
        library::clean_name(o.product, fw_hint, sizeof fw_hint);  // the name window's suggestion
        wait_step = 2;
        ask_t0 = millis();
        draw_wait("Connecting...", HINT, DIM, "ESC cancel");
        c = engine::soft_key(&info, wait_tick, err, sizeof err);
        if (c == engine::CONN_CANCELLED) modes::restart_into(modes::FW_MANAGER);
        wait_step = 1;
    } else if (c == engine::CONN_OK) {
        snprintf(found_line, sizeof found_line, "Found: download mode (4C4A:8057)");
        snprintf(ask_line, sizeof ask_line, "Already in download mode");
    }
    switch (c) {
        case engine::CONN_OK: break;
        case engine::CONN_NO_DEVICE: connect_failed("No device", "Nothing on USB in 2 minutes. Check the cable, switch it on.");
        case engine::CONN_LOADER_FAILED: connect_failed("Loader failed", "Download mode, but the loader did not start. Use Recovery.");
        case engine::CONN_HOST_ERROR: connect_failed("USB host failed", "ENTER restarts the Cardputer and tries again.");
        default: connect_failed("No answer", "Not answering over MIDI. Use Recovery.");
    }
    wait_step = 3;
    snprintf(done_line, sizeof done_line, "Connected " G_MID " %s", unit.name);
    draw_wait("Connected", "", DIM, "");
    delay(700);
    if (unit.model != engine::MODEL_FM1 && unit.model != engine::MODEL_SMK37) {
        ulog("Not an FM-1 or SMK-37 (%s): nothing for the firmware manager.", unit.why);
        begin("FIRMWARE MANAGER", FM);
        line(BODY_Y, 16, BOX, BAD, "Not an FM-1 or SMK-37");
        para(BODY_Y + 20, 13, MSG, FG, "The firmware manager is for those two. Nothing was changed.", 3);
        foot("ENTER restart it, home");
        end();
        ui::wait_released();
        for (;;) {
            int k = nav(ui::read_input());
            if (k == '\n' || k == K_BACK) exit_device();
            delay(15);
        }
    }
}

void run() {
    if (!connect_screen()) return;
    connecting();
    // the unit's folder in the library, what it holds
    busy("FIRMWARE MANAGER", FM, "Reading the device...", "Its library on the SD card.");
    char err[96];
    if (!libsd::identify(info, is_smk37(), unit.name, &lu, err, sizeof err)) {
        ulog("Reading the unit failed: %s", err);
        notice("FIRMWARE MANAGER", FM, "Cannot read it", BAD, err, "Use Recovery.");
        exit_device();
    }
    read_dev_crc();
    reload();
    main_menu();
}

}  // namespace fwman
