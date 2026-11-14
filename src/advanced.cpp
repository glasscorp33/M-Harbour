// The advanced mode: the log on screen, every step under the user's control,
// the self-tests and the hand-off to a PC.
#include "advanced.h"

#include "engine.h"
#include "library.h"
#include "library_sd.h"
#include "modes.h"
#include "restore.h"
#include "sd.h"
#include "switcher.h"
#include "uboot_host.h"
#include "ui.h"
#include "writer.h"

using engine::unit;

namespace advanced {

static constexpr uint32_t KEY_TIMEOUT_MS = 60000;

static void idle_screen() {
    ui::clear_log();
    ui::console("Advanced", TFT_CYAN);
    ulog("FM-1/SMK-37 OFF, connect to the USB-C.");
    ulog("Run on battery: no PC/charger attached.");
    ulog("ENTER/G0  connect (backup, rescue)");
    ulog("H         key only, then move to a PC");
    ulog("P / T     pin check / pulse test");
    ulog("          (nothing plugged in)");
    ulog("ESC       back to the guided mode");
    ulog("Synth rescue %s", APP_VERSION);
    ui::status(sd_ok ? "SD card ready" : "No SD card: backups need one");
}

static uint32_t key_start_ms;

static bool key_poll(const usbkey::KeyStats *s) {
    static uint32_t last_draw;
    uint32_t now = millis();
    if (now - last_draw > 500) {
        last_draw = now;
        ui::status("%lu packets %lus line %s", (unsigned long)s->packets,
                   (unsigned long)((now - key_start_ms) / 1000), s->alive ? "alive" : "-");
        vTaskDelay(1);
    }
    return ui::any_key_cancel();
}

static void calibrating(bool) { ui::console("Calibrating", TFT_YELLOW); }
static bool g0_cancel() { return ui::g0_pressed(); }

// Key and calibration. True when the ROM answered and calibrated.
static bool enter_download_mode() {
    ui::clear_log();
    ui::console("Sending key", TFT_YELLOW);
    ulog("Switch the unit ON now.");
    ulog("Any key or G0 cancels.");
    key_start_ms = millis();
    usbkey::KeyStats ks;
    engine::EnterResult r = engine::enter_download_mode({key_poll, calibrating, g0_cancel}, KEY_TIMEOUT_MS, &ks);
    switch (r) {
        case engine::ENTER_OK:
            return true;
        case engine::ENTER_ALREADY_ON:
            ui::console("D+ is high", TFT_ORANGE);
            ulog("Switch the unit off and try again.");
            return false;
        case engine::ENTER_TIMEOUT:
            ui::console("No answer", TFT_RED);
            ulog(ks.alive ? "A unit was seen: was it OFF at the start?" : "Check the cable and the unit's battery.");
            return false;
        case engine::ENTER_CANCELLED:
            ui::console("Cancelled", TFT_ORANGE);
            ui::wait_released();
            return false;
        default:
            ui::console("Failed", TFT_RED);
            return false;
    }
}

static void run_handoff() {
    if (!enter_download_mode()) return;
    ui::console("MOVE CABLE TO PC", TFT_GREEN);
    ulog("Move the cable from the Cardputer to the");
    ulog("PC now, keeping the unit on.");
    ulog("Expect 4C4A:8057 WL82 UBOOT1.00");
    ui::status("");
}

// Waits for one of the keys in `keys` (27 = ESC), keeping the session busy.
static int choose(const char *keys) {
    ui::wait_released();
    uint32_t last_ping = millis();
    for (;;) {
        int k = ui::read_key();
        if (k && strchr(keys, k)) return k;
        delay(20);
        if (millis() - last_ping > 1000) {
            last_ping = millis();
            if (!engine::keepalive()) return 0;
        }
    }
}

static void read_progress(uint32_t done, uint32_t total) {
    ui::status("Reading %3lu%%", (unsigned long)(done * 100 / total));
}
static void compare_progress(uint32_t done, uint32_t total) {
    ui::status("Comparing %lu/%lu sectors", (unsigned long)done, (unsigned long)total);
}
// Reading the backup on the card: the unit is kept busy meanwhile.
static void check_progress(uint32_t done, uint32_t total) {
    static uint32_t last_ping;
    ui::status("Checking the backup %3lu%%", (unsigned long)(done * 100 / total));
    if (millis() - last_ping > 1000) {
        last_ping = millis();
        engine::keepalive();
    }
}
static void write_progress(uint32_t done, uint32_t total) {
    ui::status("Writing %lu/%lu sectors - keep it on", (unsigned long)done, (unsigned long)total);
}

// This session's first backup: the unit as it was before anything was written
// (a restore makes one if there is none yet).
static char session_bak[48];

static bool backup(const jl::Info &info, char *path = nullptr, size_t n = 0, bool *smk = nullptr) {
    char own[48];
    if (!path) {
        path = own;
        n = sizeof own;
    }
    bool ok = engine::backup(info, path, n, smk, read_progress);
    if (ok && !session_bak[0]) snprintf(session_bak, sizeof session_bak, "%s", path);
    ui::status(ok ? "Backup done" : "Backup FAILED");
    return ok;
}

static void rescue(const jl::Info &info) {
    char err[128], bak[48];
    ui::clear_log();
    ui::console("Install rescue", TFT_ORANGE);
    bool fm1 = unit.model == engine::MODEL_FM1, smk37 = unit.model == engine::MODEL_SMK37;
    if (info.chip_key != 0x980F || !(fm1 || smk37)) {
        ulog("Unknown unit (key %04X, flash %06lX):", info.chip_key, (unsigned long)info.flash_id);
        ulog("no rescue image for it.");
        return;
    }
    const writer::Image &img = writer::image(smk37 ? writer::SMK37 : writer::FM1);
    if (!writer::image_ok(img, err, sizeof err)) {
        ulog("%s", err);
        return;
    }
    ulog("%s: m-bootloader 2.3 only. Everything", img.name);
    ulog("else up to 0xFEFFF is ERASED (the old");
    ulog("firmware, presets, settings); it then");
    ulog("waits in MIDI mode for a firmware.");
    if (smk37) ulog("Kept: 0xC0000-0xC1FFF (calibration,");
    ulog(smk37 ? "Bluetooth), 0xFF000-. Backup first." : "Kept: 0xFF000-. A backup comes first.");
    ui::status("ENTER continue  ESC cancel");
    if (choose("\n\x1b") != '\n') {
        ulog("Cancelled: nothing written.");
        return;
    }
    // A backup first, if there is a card. Without one the unit is usually
    // past saving anyway: the rescue may go on, once that is understood.
    bool have_backup = sd_ready();
    if (have_backup) {
        bool smk_marks = false;
        if (!backup(info, bak, sizeof bak, &smk_marks)) {
            ulog("No valid backup: nothing written.");
            ulog("(Without a card the rescue can go on");
            ulog("with no backup.)");
            return;
        }
        if (fm1 && smk_marks) {
            ulog("SMK-37 calibration data found: this is");
            ulog("not an FM-1. Nothing written.");
            return;
        }
    } else {
        ui::clear_log();
        ui::console("No SD card", TFT_RED);
        ulog("No backup is possible. What the unit");
        ulog("holds now (firmware, settings, presets)");
        ulog("is lost for good if you go on.");
        ulog("Insert a card (FAT32) to keep a backup.");
        ui::status("C continue, no backup  ESC cancel");
        if (choose("c\x1b") != 'c') {
            ulog("Cancelled: nothing written.");
            return;
        }
        have_backup = sd_ready();  // a card went in meanwhile: back up after all
        if (have_backup && !backup(info, bak, sizeof bak)) {
            ulog("No valid backup: nothing written.");
            return;
        }
    }
    ui::status("Comparing the unit with the image...");
    writer::Plan plan;
    if (!writer::plan(img, &plan, compare_progress, err, sizeof err)) {
        ulog("Reading the unit failed: %s", err);
        return;
    }
    ui::clear_log();
    ui::console("Ready to write", TFT_ORANGE);
    if (have_backup) ulog("%s rescue. Backup: %s", img.name, bak + 5);
    else ulog("%s rescue. NO BACKUP.", img.name);
    if (!plan.to_write && !plan.to_erase) {
        ulog("The unit already holds the image.");
        return;
    }
    ulog("%lu sectors to write, %lu to erase,", (unsigned long)plan.to_write, (unsigned long)plan.to_erase);
    ulog("in %lu runs.", (unsigned long)plan.runs);
    ulog("Do not switch the unit off or unplug it");
    ulog("while writing.");
    ui::status("W write now  ESC cancel");
    if (choose("w\x1b") != 'w') {
        ulog("Cancelled: nothing written.");
        return;
    }
    ui::clear_log();
    ui::console("Writing", TFT_YELLOW);
    uint32_t t0 = millis();
    if (!writer::apply(img, plan, ulog, write_progress, err, sizeof err)) {
        ui::console("WRITE FAILED", TFT_RED);
        ulog("%s", err);
        ulog("Do NOT switch the unit off.");
        if (have_backup) ulog("Its backup: %s", bak);
        ui::status("W retry  R restart unit  ESC end");
        return;
    }
    ui::console("Rescue installed", TFT_GREEN);
    ulog("Done in %lu s. R restarts the unit.", (unsigned long)((millis() - t0) / 1000));
}

// ---------------------------------------------------------------------------
// Restore a backup: the unit exactly as it was then (its own bootloader,
// firmware, settings), all but the ranges the rescue never writes either.

// n: backup-NNN, picked on the library screen.
static void restore_backup(const jl::Info &info, int n) {
    char err[128];
    ui::clear_log();
    ui::console("Restore backup", TFT_ORANGE);
    bool fm1 = unit.model == engine::MODEL_FM1, smk37 = unit.model == engine::MODEL_SMK37;
    if (info.chip_key != 0x980F || !(fm1 || smk37)) {
        ulog("Unknown unit (key %04X, flash %06lX):", info.chip_key, (unsigned long)info.flash_id);
        ulog("no restore for it.");
        return;
    }
    if (!sd_ready()) {
        ulog("No SD card: insert the one with the");
        ulog("backups (FAT32), then L again.");
        return;
    }

    // whole, intact and this unit's: refused otherwise, before anything else
    static writer::Source src;
    static char name[16];
    uint32_t size = 0;
    snprintf(name, sizeof name, "backup-%03d", n);
    ui::clear_log();
    ui::console("Checking", TFT_YELLOW);
    ulog("Restore %s.bin: checking it", name);
    if (!engine::open_backup(n, &src, &size)) {
        ulog("Cannot open %s.bin.", name);
        return;
    }
    restore::Notes notes;
    bool has_notes = engine::backup_notes(n, &notes);
    restore::Result r;
    if (!restore::check(info, smk37, size, has_notes ? &notes : nullptr, src, check_progress, &r, err, sizeof err)) {
        ulog("Reading the unit failed: %s", err);
        engine::close_backup();
        return;
    }
    ulog("%s: %s", name, r.why);
    if (!restore::allowed(r.verdict)) {
        ui::console("Refused", TFT_RED);
        if (r.verdict == restore::BAD_FILE) ulog("Not a whole, intact backup.");
        else if (r.verdict == restore::OTHER_UNIT) ulog("It is not this unit's backup.");
        else ulog("Nothing shows it is this unit's.");
        ulog("Nothing written.");
        ui::status("Refused: nothing written");
        engine::close_backup();
        return;
    }

    // what it holds, and what would change
    const writer::Image &rescue_img = writer::image(smk37 ? writer::SMK37 : writer::FM1);
    writer::Image img = writer::full_image(smk37 ? writer::SMK37 : writer::FM1, name, &src);
    writer::Plan plan;
    ui::status("Comparing the unit with the backup...");
    if (!writer::plan(img, &plan, compare_progress, err, sizeof err)) {
        ulog("Comparing failed: %s", err);
        ulog("Nothing written.");
        engine::close_backup();
        return;
    }
    ui::clear_log();
    ui::console("Restore backup", TFT_ORANGE);
    ulog("%s, CRC32 %08lX, this unit's", name, (unsigned long)r.crc);
    ulog(r.verdict == restore::SAME_BY_ID ? "(same unit ID)." : "(same factory data; no unit ID).");
    ulog("Boot area: %s", r.boot_crc == rescue_img.boot_crc ? "the rescue's m-bootloader" : "not the rescue's");
    ulog("The unit becomes as it was then:");
    ulog("bootloader, firmware, settings.");
    ulog(smk37 ? "Kept: 0xC0000-0xC1FFF (pairings, calib-" : "Kept: 0xFF000-0xFFFFF (key_mac).");
    if (smk37) ulog("ration), 0xFF000-0xFFFFF (key_mac).");
    if (!plan.to_write && !plan.to_erase) {
        ulog("The unit already holds this backup.");
        ui::status("Nothing to write");
        engine::close_backup();
        return;
    }
    ulog("%lu sectors to write, %lu to erase.", (unsigned long)plan.to_write, (unsigned long)plan.to_erase);
    ui::status("ENTER continue  ESC cancel");
    if (choose("\n\x1b") != '\n') {
        ulog("Cancelled: nothing written.");
        engine::close_backup();
        return;
    }

    // the unit as it is now kept first, unless this session has a backup already
    if (!session_bak[0]) {
        ui::clear_log();
        ui::console("Backup first", TFT_CYAN);
        ulog("A backup of the unit as it is now.");
        if (!backup(info)) {
            ulog("No valid backup: nothing written.");
            engine::close_backup();
            return;
        }
    }
    ui::clear_log();
    ui::console("Ready to write", TFT_ORANGE);
    ulog("Restore %s. As it was: %s", name, session_bak + 5);
    ulog("%lu sectors to write, %lu to erase,", (unsigned long)plan.to_write, (unsigned long)plan.to_erase);
    ulog("in %lu runs; the boot area last.", (unsigned long)plan.runs);
    ulog("Do not switch the unit off or unplug it");
    ulog("while writing.");
    ui::status("W write now  ESC cancel");
    if (choose("w\x1b") != 'w') {
        ulog("Cancelled: nothing written.");
        engine::close_backup();
        return;
    }
    ui::clear_log();
    ui::console("Writing", TFT_YELLOW);
    ulog("Restoring %s", name);
    uint32_t t0 = millis();
    bool ok = writer::apply(img, plan, ulog, write_progress, err, sizeof err);
    engine::close_backup();
    if (!ok) {
        ui::console("RESTORE FAILED", TFT_RED);
        ulog("%s", err);
        ulog("Do NOT switch the unit off.");
        ulog("As it was: %s", session_bak);
        ulog("L tries again (the card back in, if");
        ulog("it was the card); R restarts the unit.");
        return;
    }
    ui::console("Backup restored", TFT_GREEN);
    ulog("Done in %lu s. R restarts the unit.", (unsigned long)((millis() - t0) / 1000));
}

// A unit with no rescue image and no library (not an FM-1 or SMK-37):
// a backup, a restart, the end.
static void connected_menu(const jl::Info &info) {
    for (;;) {
        ui::status("ENTER backup  R restart unit  ESC end");
        uint32_t last_ping = millis();
        int k = 0;
        while (!(k = ui::read_key())) {
            delay(20);
            if (millis() - last_ping > 1000) {  // keep the session busy
                last_ping = millis();
                if (!engine::keepalive()) {
                    ulog("The unit was disconnected.");
                    return;
                }
            }
        }
        if (k == '\n') {
            backup(info);
            ui::wait_released();
            delay(1500);
        } else if (k == 'w' || k == 'l') {
            ulog("No rescue or restore for an %s.", unit.name);
        } else if (k == 'r') {
            char err[96];
            ulog(jl::restart(err, sizeof err) ? "Restarted: the unit boots its firmware." : "Restart failed");
            return;
        } else if (k == 27) {
            ulog("Left in download mode.");
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// The library (docs/FIRMWARE-MANAGER.md, phase 2): this unit's snapshots,
// SAVE, LOAD (switch), RENAME, DELETE, IMPORT of an old backup-NNN.

static char fw_hint[48];  // the USB product name the unit gave before the soft key, or ""
static libsd::Unit lib_unit;
static library::Entry entries[libsd::MAX_ENTRIES];
static ui::Row rows[libsd::MAX_ENTRIES];
static int n_entries, sel, first_row;
static bool unit_gone;

// Keeps the session busy; false (and remembered) once the unit is gone.
static bool session_tick() {
    if (!unit_gone && !engine::keepalive()) {
        unit_gone = true;
        ulog("The unit was disconnected.");
    }
    return !unit_gone;
}

static bool is_smk37() { return unit.model == engine::MODEL_SMK37; }

// The list again from the card, the selection kept in range.
static void reload(const jl::Info &info) {
    n_entries = libsd::list(lib_unit, info, entries, libsd::MAX_ENTRIES);
    for (int i = 0; i < n_entries; i++) {
        const library::Entry &e = entries[i];
        ui::Row &r = rows[i];
        if (e.kind == library::SNAPSHOT) {
            snprintf(r.text, sizeof r.text, "%c %04d %s", e.current ? '*' : ' ', e.number, e.name);
            r.color = e.current ? TFT_GREEN : e.source == library::SRC_AUTO ? TFT_LIGHTGREY : TFT_WHITE;
        } else {
            snprintf(r.text, sizeof r.text, "  old backup-%03d  %s", e.number, library::whose_name(e.whose));
            r.color = e.whose == library::WHOSE_OTHER ? ui::RED : TFT_DARKGREY;
        }
    }
    if (sel >= n_entries) sel = n_entries - 1;
    if (sel < 0) sel = 0;
    if (sel < first_row) first_row = sel;
    if (sel >= first_row + ui::LIST_ROWS) first_row = sel - ui::LIST_ROWS + 1;
    if (first_row < 0) first_row = 0;
}

static void draw_library() {
    char sub[48], detail[48];
    snprintf(sub, sizeof sub, "%s %s", unit.name, lib_unit.file.label[0] ? lib_unit.file.label : lib_unit.id);
    if (!sd_ok) {
        snprintf(detail, sizeof detail, "No SD card: the library needs one.");
    } else if (!n_entries) {
        snprintf(detail, sizeof detail, "No snapshots yet: ENTER saves one.");
    } else {
        const library::Entry &e = entries[sel];
        if (e.kind == library::LEGACY)
            snprintf(detail, sizeof detail, "old backup: L restores it all, I imports");
        else if (!e.has_crc)
            snprintf(detail, sizeof detail, "no .txt: cannot be loaded");
        else
            snprintf(detail, sizeof detail, "%s%s%08lx %s", e.current ? "current, " : "",
                     e.source == library::SRC_AUTO ? "auto " : e.source == library::SRC_IMPORT ? "import " : "",
                     (unsigned long)e.crc, e.model);
    }
    ui::list_screen("Library", TFT_CYAN, sub, rows, n_entries, first_row, sel, detail,
                    "ENTER save L load N rename D del ;. move",
                    "I import U label W rescue V log R ESC");
}

// After an action on the console: any key back to the library.
static void wait_back() {
    ui::status("Any key: back to the library");
    ui::wait_released();
    uint32_t last = millis();
    while (!ui::read_key()) {
        delay(20);
        if (millis() - last > 1000) {
            last = millis();
            if (!session_tick()) return;
        }
    }
    ui::wait_released();
}

static bool name_window(const char *title, const char *help1, char *name) {
    return ui::text_input(title, help1, "No date is known: type it in the name.", name, library::NAME_CHARS + 1,
                          session_tick);
}

// Waits for one of `keys` with the console up; 0 when the unit is gone.
static int ask(const char *keys) {
    int k = choose(keys);
    if (!k) unit_gone = true;
    return k;
}

static bool battery_hook(void *, char *why, size_t n) {
    int mv = M5.Power.getBatteryVoltage(), pct;
    library::Battery b = library::battery(mv, &pct);
    if (b == library::BATTERY_UNKNOWN) {
        ulog("Battery: no believable reading (%d mV): not checked", mv);
        return true;
    }
    ulog("Cardputer battery %d %% (%d mV)", pct, mv);
    if (b == library::BATTERY_OK) return true;
    snprintf(why, n, "Cardputer battery %d %%, under %d %%: charge it first", pct, library::BATTERY_MIN_PERCENT);
    return false;
}

static void save_snapshot(const jl::Info &info) {
    char name[library::NAME_CHARS + 1];
    library::suggest(fw_hint, libsd::today(), unit.name, name, sizeof name);
    if (!name_window("Save snapshot", "A name for the unit as it is now.", name)) return;
    ui::clear_log();
    ui::console("Saving", TFT_CYAN);
    ulog("Save \"%s\"", name);
    int num;
    if (libsd::save(&lib_unit, info, name, fw_hint, library::SRC_BACKUP, &num, read_progress)) {
        libsd::set_current(lib_unit, num, name);
        ui::console("Saved", TFT_GREEN);
    } else {
        ui::console("Not saved", TFT_RED);
    }
    wait_back();
}

// --- LOAD

struct LoadCtx {
    const jl::Info *info;
    const library::Entry *e;
    char autosave[64];  // what the auto-save did
};

static bool autosave_hook(void *ctx, char *err, size_t errlen) {
    LoadCtx *c = (LoadCtx *)ctx;
    ui::clear_log();
    ui::console("Auto-save", TFT_CYAN);
    ulog("First the unit as it is now:");
    if (!libsd::auto_save(&lib_unit, *c->info, fw_hint, c->autosave, sizeof c->autosave, read_progress)) {
        snprintf(err, errlen, "the auto-save could not be made");
        return false;
    }
    ulog("Auto-save: %s", c->autosave);
    ui::console("Checking", TFT_YELLOW);
    return true;
}

static switcher::Choice confirm_hook(void *ctx, int stage, const switcher::Report &r) {
    LoadCtx *c = (LoadCtx *)ctx;
    ui::clear_log();
    if (stage == 0) {
        ui::console("Load: ready", TFT_ORANGE);
        ulog("%04d %s, CRC32 %08lX,", c->e->number, c->e->name, (unsigned long)r.check.crc);
        ulog(r.check.verdict == restore::SAME_BY_ID ? "this unit's (same unit ID)." : "this unit's (factory data).");
        ulog("Auto-save: %s", c->autosave);
        ulog("Boot area kept (0x0-0x3FFF: the unit's).");
        if (r.boot_differs) ulog("The snapshot's boot area differs.");
        ulog("%lu sectors to write, %lu to erase.", (unsigned long)r.plan.to_write, (unsigned long)r.plan.to_erase);
        ulog(is_smk37() ? "Never written: 0xC0000-0xC1FFF, 0xFF000-." : "Never written: 0xFF000-0xFFFFF.");
        ulog("Do not switch anything off while writing.");
        ui::status(r.boot_differs ? "W write  X exactly  ESC cancel" : "W write now  ESC cancel");
        int k = ask(r.boot_differs ? "wx\x1b" : "w\x1b");
        return k == 'w' ? switcher::WRITE : k == 'x' ? switcher::EXACT : switcher::CANCEL;
    }
    ui::console("Restore exactly?", TFT_RED);
    ulog("WARNING: this writes the boot area");
    ulog("(0x0-0x3FFF) too: the riskiest write.");
    ulog("It is written last; if the write stops");
    ulog("there the unit may need the rescue (W).");
    ulog("Only if the snapshot needs its own boot.");
    ulog("%lu sectors to write, %lu to erase.", (unsigned long)r.plan.to_write, (unsigned long)r.plan.to_erase);
    ui::status("Y write exactly  ESC cancel");
    return ask("y\x1b") == 'y' ? switcher::WRITE : switcher::CANCEL;
}

static void load_progress(uint32_t done, uint32_t total) {
    static uint32_t last_ping;
    ui::status("Working %3lu%%  keep both on", (unsigned long)(done * 100 / (total ? total : 1)));
    if (millis() - last_ping > 1000) {
        last_ping = millis();
        engine::keepalive();
    }
}

// True: the session ends (the unit was restarted).
static bool load_snapshot(const jl::Info &info, const library::Entry &e) {
    if (e.kind == library::LEGACY) {  // an old backup: the restore, exactly as before
        restore_backup(info, e.number);
        wait_back();
        return false;
    }
    ui::clear_log();
    ui::console("Load", TFT_ORANGE);
    if (!e.has_crc) {
        ulog("%s has no .txt with a CRC32:", e.base);
        ulog("it cannot be checked, so not loaded.");
        wait_back();
        return false;
    }
    ulog("Load %04d %s", e.number, e.name);
    ulog("1. auto-save of the unit as it is now");
    ulog("   (not stored twice if it equals one)");
    ulog("2. checks: this unit's, whole, intact");
    ulog("3. what differs, then W to write; the");
    ulog("   boot area is kept unless you ask.");
    ui::status("ENTER go on  ESC cancel");
    if (ask("\n\x1b") != '\n') {
        ulog("Cancelled: nothing written.");
        return false;
    }
    static writer::Source src;
    static library::Meta meta;
    char path[128], err[128];
    uint32_t size = 0;
    libsd::bin_path(lib_unit, e, path, sizeof path);
    bool has_meta = libsd::read_meta(lib_unit, e, &meta);
    if (!engine::open_file(path, &src, &size)) {
        ulog("Cannot open %s", path);
        wait_back();
        return false;
    }
    LoadCtx ctx = {&info, &e, ""};
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
    h.auto_save = autosave_hook;
    h.confirm = confirm_hook;
    h.log = ulog;
    h.progress = load_progress;
    static switcher::Report rep;
    uint32_t t0 = millis();
    switcher::Outcome o = switcher::load(req, h, &rep, err, sizeof err);
    engine::close_backup();
    ulog("Load: %s", switcher::outcome_name(o));
    switch (o) {
        case switcher::WRITTEN:
        case switcher::NOTHING_TO_DO:
            libsd::set_current(lib_unit, e.number, e.name);
            ui::console(o == switcher::WRITTEN ? "Loaded" : "Already loaded", TFT_GREEN);
            if (o == switcher::WRITTEN) ulog("Done in %lu s.", (unsigned long)((millis() - t0) / 1000));
            else ulog("The unit already holds it.");
            ulog("R restarts the unit into it now.");
            ui::status("R restart the unit  ESC library");
            if (ask("r\x1b") == 'r') {
                ulog(jl::restart(err, sizeof err) ? "Restarted: the unit boots its firmware." : "Restart failed");
                return true;
            }
            return false;
        case switcher::WRITE_FAILED:
            ui::console("LOAD FAILED", TFT_RED);
            ulog("%s", err);
            ulog("Do NOT switch the unit off. L again");
            ulog("finishes it (the card back in, if it");
            ulog("was the card). Before: %s", ctx.autosave);
            break;
        case switcher::REFUSED:
            ui::console("Refused", TFT_RED);
            ulog("%s", err);
            ulog("Nothing written.");
            break;
        case switcher::BATTERY_LOW:
            ui::console("Battery low", TFT_RED);
            ulog("%s.", err);
            ulog("Nothing written.");
            break;
        case switcher::CANCELLED:
            ui::console("Cancelled", TFT_ORANGE);
            ulog("Nothing written.");
            if (ctx.autosave[0]) ulog("Auto-save: %s", ctx.autosave);
            break;
        default:
            ui::console(o == switcher::AUTOSAVE_FAILED ? "No auto-save" : "Read failed", TFT_RED);
            if (err[0]) ulog("%s", err);
            ulog("Nothing written.");
            break;
    }
    wait_back();
    return false;
}

// --- RENAME, DELETE, IMPORT, the unit's label

static void rename_snapshot(const library::Entry &e) {
    if (e.kind != library::SNAPSHOT) {
        ui::clear_log();
        ui::console("Rename", TFT_ORANGE);
        ulog("Old backups keep their file name:");
        ulog("I imports one into the library with a");
        ulog("name of its own.");
        wait_back();
        return;
    }
    char name[library::NAME_CHARS + 1];
    snprintf(name, sizeof name, "%s", e.name);
    if (!name_window("Rename", e.base, name)) return;
    ui::clear_log();
    ui::console("Rename", TFT_CYAN);
    if (!libsd::rename(lib_unit, e, name)) {
        ui::console("Not renamed", TFT_RED);
        wait_back();
    }
}

static void delete_snapshot(const library::Entry &e) {
    if (e.kind != library::SNAPSHOT) {
        ui::clear_log();
        ui::console("Delete", TFT_ORANGE);
        ulog("Old backups are not deleted here");
        ulog("(on a PC if you must).");
        wait_back();
        return;
    }
    ui::clear_log();
    ui::console("Delete?", TFT_RED);
    ulog("%04d %s", e.number, e.name);
    if (e.current) ulog("It is the current one.");
    ulog("Its .bin and .txt are deleted for good.");
    ui::status("Y delete  ESC keep");
    if (ask("y\x1b") != 'y') return;
    if (library::count_snapshots(entries, n_entries) == 1) {
        ui::clear_log();
        ui::console("Delete the ONLY one?", TFT_RED);
        ulog("It is this unit's only snapshot:");
        ulog("there is no other copy of the unit in");
        ulog("the library afterwards.");
        ui::status("Y delete it anyway  ESC keep");
        if (ask("y\x1b") != 'y') return;
    }
    ui::console("Deleting", TFT_ORANGE);
    if (!libsd::remove(lib_unit, e)) wait_back();
}

static void import_backup(const jl::Info &info, const library::Entry &e) {
    ui::clear_log();
    ui::console("Import", TFT_CYAN);
    if (e.kind != library::LEGACY) {
        ulog("Only old backups (backup-NNN) are");
        ulog("imported: pick one lower in the list.");
        wait_back();
        return;
    }
    // the same checks as before a restore: whole, intact, this unit's
    static writer::Source src;
    static library::Meta meta;
    char path[64], err[128];
    uint32_t size = 0;
    libsd::bin_path(lib_unit, e, path, sizeof path);
    bool has_meta = libsd::read_meta(lib_unit, e, &meta);
    ulog("backup-%03d: checking it", e.number);
    if (!engine::open_file(path, &src, &size)) {
        ulog("Cannot open %s", path);
        wait_back();
        return;
    }
    restore::Result r;
    bool read = restore::check(info, is_smk37(), size, has_meta ? &meta.notes : nullptr, src, check_progress, &r, err,
                               sizeof err);
    engine::close_backup();
    if (!read) {
        ulog("Reading the unit failed: %s", err);
        wait_back();
        return;
    }
    ulog("backup-%03d: %s", e.number, r.why);
    if (!restore::allowed(r.verdict) || !library::same_model(unit.name, meta.model)) {
        ui::console("Refused", TFT_RED);
        ulog(r.verdict == restore::BAD_FILE ? "Not a whole, intact backup." : "It is not this unit's backup.");
        ulog("Not imported.");
        wait_back();
        return;
    }
    char name[library::NAME_CHARS + 1];
    snprintf(name, sizeof name, "backup-%03d", e.number);
    if (!name_window("Import: name", "A name for it in the library.", name)) return;
    ui::clear_log();
    ui::console("Importing", TFT_CYAN);
    int num;
    bool ok = libsd::import(&lib_unit, info, e, name, &num, read_progress);
    ui::console(ok ? "Imported" : "Not imported", ok ? TFT_GREEN : TFT_RED);
    wait_back();
}

static void label_unit(const jl::Info &info) {
    char label[library::NAME_CHARS + 1];
    if (lib_unit.file.label[0]) snprintf(label, sizeof label, "%s", lib_unit.file.label);
    else snprintf(label, sizeof label, "My %s", unit.name);
    if (!ui::text_input("Unit label", lib_unit.id, "Shown on the library screen.", label, sizeof label,
                        session_tick))
        return;
    ui::clear_log();
    ui::console("Label", TFT_CYAN);
    if (!libsd::set_label(&lib_unit, info, label)) wait_back();
}

static int read_library_key() {
    uint32_t last = millis();
    for (;;) {
        int k = ui::read_input();
        if (k) return k;
        delay(20);
        if (millis() - last > 1000) {
            last = millis();
            if (!session_tick()) return 0;
        }
    }
}

static void library_menu(const jl::Info &info) {
    char err[96];
    unit_gone = false;
    sel = first_row = 0;
    if (!libsd::identify(info, is_smk37(), unit.name, &lib_unit, err, sizeof err)) {
        ulog("Reading the unit failed: %s", err);
        connected_menu(info);
        return;
    }
    bool dirty = true;  // the card may have changed: list it again
    for (;;) {
        if (dirty) reload(info);
        dirty = true;
        if (sel < first_row) first_row = sel;
        if (sel >= first_row + ui::LIST_ROWS) first_row = sel - ui::LIST_ROWS + 1;
        draw_library();
        ui::wait_released();
        int k = read_library_key();
        if (!k) return;  // the unit is gone
        if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
        const library::Entry *e = n_entries ? &entries[sel] : nullptr;
        bool move = k == ui::IN_UP || k == ';' || k == ui::IN_DOWN || k == '.' || k == ui::IN_LEFT || k == ',' ||
                    k == ui::IN_RIGHT || k == '/';
        if (move) dirty = false;
        if (k == ui::IN_UP || k == ';') {
            if (sel > 0) sel--;
        } else if (k == ui::IN_DOWN || k == '.') {
            if (sel + 1 < n_entries) sel++;
        } else if (k == ui::IN_LEFT || k == ',') {
            sel = sel > ui::LIST_ROWS ? sel - ui::LIST_ROWS : 0;
        } else if (k == ui::IN_RIGHT || k == '/') {
            sel = sel + ui::LIST_ROWS < n_entries ? sel + ui::LIST_ROWS : (n_entries ? n_entries - 1 : 0);
        } else if (k == '\n') {
            save_snapshot(info);
        } else if (k == 'l' && e) {
            library::Entry copy = *e;
            if (load_snapshot(info, copy)) return;
        } else if (k == 'n' && e) {
            library::Entry copy = *e;
            rename_snapshot(copy);
        } else if (k == 'd' && e) {
            library::Entry copy = *e;
            delete_snapshot(copy);
        } else if (k == 'i' && e) {
            library::Entry copy = *e;
            import_backup(info, copy);
        } else if (k == 'u') {
            label_unit(info);
        } else if (k == 'w') {
            rescue(info);
            wait_back();
        } else if (k == 'v') {
            ui::show_console();
            wait_back();
        } else if (k == 'r') {
            ui::clear_log();
            ui::console("Restart", TFT_CYAN);
            ulog(jl::restart(err, sizeof err) ? "Restarted: the unit boots its firmware." : "Restart failed");
            return;
        } else if (k == ui::IN_ESC || k == ui::IN_DEL) {
            ui::clear_log();
            ui::console("Library", TFT_CYAN);
            ulog("Left in download mode.");
            return;
        }
        if (unit_gone) {
            ui::console("Disconnected", TFT_RED);
            return;
        }
    }
}
static uint32_t soft_key_start;

static bool soft_key_tick() {
    static uint32_t last_draw;
    uint32_t now = millis();
    if (now - last_draw > 500) {
        last_draw = now;
        ui::status("Waiting %lus / %lus  any key cancels", (unsigned long)((now - soft_key_start) / 1000),
                   (unsigned long)(engine::SOFT_KEY_MS / 1000));
    }
    return ui::any_key_cancel();
}

// A device that is not the download mode (a unit running its firmware, ...):
// only for USB MIDI, and only on S, the soft key. No keepalive here: the
// loader's ping would be sent to the device. True: connected after all.
static bool offer_soft_key(jl::Info *info, char *err, size_t errlen) {
    const engine::Other &o = engine::other;
    ui::console("Not download mode", TFT_ORANGE);
    ulog("%04X:%04X \"%s\" is running.", o.vid, o.pid, o.product[0] ? o.product : "no product name");
    if (o.midi != usbmidi::FOUND) {
        ulog("Not USB MIDI (%s):", usbmidi::find_name(o.midi));
        ulog("nothing to send it. Switch it off and");
        ulog("try again with the key at power-on.");
        return false;
    }
    ulog("USB MIDI: interface %u, OUT EP %02X.", o.out.iface, o.out.ep);
    ulog("S  send the download-mode request over");
    ulog("   MIDI (F0 22 24 35 7D F7, M-Flasher's");
    ulog("   soft key; nothing is written)");
    ulog("ESC end (switch it off, try again)");
    ui::status("S MIDI request  ESC end");
    ui::wait_released();
    int k;
    while ((k = ui::read_key()) != 's' && k != 27) delay(20);
    if (k != 's') {
        ulog("Nothing sent.");
        return false;
    }
    ui::wait_released();
    ui::clear_log();
    ui::console("MIDI request", TFT_YELLOW);
    soft_key_start = millis();
    engine::ConnectResult c = engine::soft_key(info, soft_key_tick, err, errlen);
    if (c == engine::CONN_OK) return true;
    switch (c) {
        case engine::CONN_NO_ANSWER: ui::console("No answer", TFT_RED); break;
        case engine::CONN_NOT_BACK: ui::console("Not back", TFT_RED); break;
        case engine::CONN_CANCELLED: ui::console("Cancelled", TFT_ORANGE); break;
        case engine::CONN_LOADER_FAILED: ui::console("Loader failed", TFT_RED); break;
        default: ui::console("MIDI failed", TFT_RED); break;
    }
    ui::wait_released();
    return false;
}

static void run_session() {
    if (!enter_download_mode()) return;
    ui::status("Connecting...");
    jl::Info info;
    char err[96];
    engine::ConnectResult c = engine::connect(&info, err, sizeof err);
    fw_hint[0] = 0;
    if (c == engine::CONN_NOT_ROM && engine::other.vid) {
        if (!offer_soft_key(&info, err, sizeof err)) return;
        // the firmware that was running: the name window's suggestion
        library::clean_name(engine::other.product, fw_hint, sizeof fw_hint);
    } else if (c != engine::CONN_OK) {
        ui::console(c == engine::CONN_LOADER_FAILED ? "Loader failed" : "No connection", TFT_RED);
        ulog("%s", err);
        return;
    }
    ui::console("Connected", TFT_GREEN);
    ulog("Unit: %s (%s)", unit.name, unit.why);
    if (unit.model == engine::MODEL_FM1 || unit.model == engine::MODEL_SMK37) library_menu(info);
    else connected_menu(info);
}

static void end_session() {
    ui::status("Any key: restart the Cardputer");
    ui::wait_released();
    while (!ui::read_key()) delay(20);
    modes::restart_into(modes::ADVANCED);  // the USB host is up: start over clean
}

void run() {
    idle_screen();
    for (;;) {
        // no card at power-on: look again now and then on the idle screen
        static uint32_t last_sd_try;
        if (!sd_ok && millis() - last_sd_try > 3000) {
            last_sd_try = millis();
            if (sd_mount()) ui::status("SD card ready");
        }
        int k = ui::read_key();
        if (k == '\n') {
            ui::wait_released();
            run_session();
            end_session();
        } else if (k == 'h') {
            ui::wait_released();
            run_handoff();
            end_session();
        } else if (k == 'p') {
            int hi, lo;
            usbkey::pin_check(&hi, &lo);
            bool ok = hi == 3 && lo == 0;
            ui::clear_log();
            ui::console(ok ? "Pin check OK" : "Pin check FAILED", ok ? TFT_GREEN : TFT_RED);
            ulog("H%d L%d (expect H3 L0)", hi, lo);
            ulog("Run with nothing plugged into USB-C.");
            ui::status("Any key: back");
            while (!ui::read_key()) delay(20);
            idle_screen();
        } else if (k == 't') {
            ui::clear_log();
            ui::console("Pulse test", TFT_YELLOW);
            ulog("Measuring for 3 s, nothing plugged in.");
            uint32_t n;
            double period;
            if (!usbkey::pulse_test(&n, &period)) {
                ui::console("Pulse test FAILED", TFT_RED);
                ulog("Could not start the pulse generator.");
            } else {
                double ppm = (period - 1000.0) * 1000.0;
                bool ok = n > 2900 && n < 3100 && ppm > -500 && ppm < 500;
                ui::console(ok ? "Pulse test OK" : "Pulse test CHECK", ok ? TFT_GREEN : TFT_ORANGE);
                ulog("%lu edges, period %.3f us", (unsigned long)n, period);
                ulog("error %+.0f ppm (USB FS allows 2500)", ppm);
            }
            ui::status("Any key: back");
            while (!ui::read_key()) delay(20);
            idle_screen();
        } else if (k == 27) {
            ui::wait_released();
            return;
        }
        delay(10);
    }
}

}  // namespace advanced
