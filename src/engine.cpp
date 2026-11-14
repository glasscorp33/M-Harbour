#include "engine.h"

#include <Arduino.h>
#include <string.h>

#include "sd.h"
#include "uboot_host.h"
#include "ui.h"

namespace engine {

static constexpr uint32_t CHUNK = 16384;
static uint8_t chunk_buf[CHUNK];
Unit unit;

// ---------------------------------------------------------------------------
// Download mode

EnterResult enter_download_mode(const EnterHooks &h, uint32_t timeout_ms, usbkey::KeyStats *ks) {
    ulog("Sending the key; switch the unit ON.");
    usbkey::KeyResult kr = usbkey::send_key(ks, timeout_ms, h.poll);
    switch (kr) {
        case usbkey::DP_HIGH_AT_START:
            ulog("D+ is high: the unit is already on.");
            return ENTER_ALREADY_ON;
        case usbkey::TIMEOUT:
            ulog("No ACK in %lu s: %lu packets, %lu false ACK, line %s.", (unsigned long)(timeout_ms / 1000),
                 (unsigned long)ks->packets, (unsigned long)ks->false_acks, ks->alive ? "alive" : "dead");
            return ENTER_TIMEOUT;
        case usbkey::CANCELLED:
            ulog("Key cancelled.");
            return ENTER_CANCELLED;
        default:
            break;
    }
    ulog("%s after %lu packets (%lu false ACK)", kr == usbkey::ANSWER_ACK ? "ACK" : "D+ up (ACK missed)",
         (unsigned long)ks->packets, (unsigned long)ks->false_acks);
    if (h.answered) h.answered(kr == usbkey::ANSWER_ACK);
    usbkey::PulseResult pr = usbkey::calibrate(h.cancel);
    if (pr.rmt_error) {
        ulog("Could not start the pulse generator.");
        return ENTER_FAILED;
    }
    if (pr.cancelled) {
        ulog("Stopped during calibration.");
        return ENTER_CANCELLED;
    }
    ulog("%lld ms of pulses, D+ %s, now %d", (long long)pr.pulse_ms,
         pr.drop_ms >= 0 ? "dropped (cal done)" : "still high", usbkey::dp());
    return ENTER_OK;
}

// ---------------------------------------------------------------------------
// Which unit

// M-Flasher's evidence (boards.rs): the SMK-37's calibration record (trimdat
// at 0xC1224, m-bootloader's TRIMCOPY at 0xFF000) decides; else the flash ID
// (FM-1 0x856014 / 0xB36014, SMK-37 0xEB6014).
static Unit identify_unit(const jl::Info &info) {
    uint8_t a[32] = {0}, b[32] = {0};
    char err[96];
    bool trimdat = jl::read(0xC1220, a, sizeof a, err, sizeof err) && memcmp(a + 4, "trimdat\0", 8) == 0;
    bool trimcopy = jl::read(0xFF000, b, sizeof b, err, sizeof err) && memcmp(b, "TRIMCOPY", 8) == 0;
    if (info.chip_key != 0x980F) return {MODEL_UNKNOWN, "unknown unit", "chip key is not 980F"};
    if (trimdat || trimcopy) return {MODEL_SMK37, "SMK-37", "its calibration record"};
    if (info.flash_id == 0xEB6014) return {MODEL_SMK37, "SMK-37", "flash ID"};
    if (info.flash_id == 0x856014 || info.flash_id == 0xB36014) return {MODEL_FM1, "FM-1", "flash ID, no SMK-37 data"};
    return {MODEL_UNKNOWN, "unknown unit", "flash ID not known"};
}

// ---------------------------------------------------------------------------
// USB host and loader

Other other;

static bool is_rom(uint16_t vid, uint16_t pid) { return vid == 0x4C4A && pid == 0x8057; }

// What the device that is not the download mode is, for the log and the soft
// key. Reads only what the USB host already has: nothing is sent to it.
static void inspect_other(uint16_t vid, uint16_t pid) {
    other = {};
    other.vid = vid;
    other.pid = pid;
    other.midi = usbmidi::BAD_DESCRIPTOR;
    host::Device d;
    if (!host::device(&d)) {
        ulog("%04X:%04X: no descriptors from the USB host", vid, pid);
        return;
    }
    snprintf(other.product, sizeof other.product, "%s", d.product);
    ulog("%04X:%04X product \"%s\"", vid, pid, d.product[0] ? d.product : "(none)");
    if (!d.config) {
        ulog("No configuration descriptor: not USB MIDI.");
        return;
    }
    other.midi = usbmidi::find_midi_out(d.config, d.config_len, &other.out);
    if (other.midi == usbmidi::FOUND)
        ulog("USB MIDI: interface %u alt %u, bulk OUT EP %02X, %u-byte packets", other.out.iface, other.out.alt,
             other.out.ep, other.out.mps);
    else
        ulog("Not USB MIDI: %s (config %u bytes)", usbmidi::find_name(other.midi), (unsigned)d.config_len);
}

// The download mode is on the bus: the loader, and which unit it is.
static ConnectResult start_loader(jl::Info *info, char *err, size_t errlen) {
    if (!jl::start(info, ulog, err, errlen)) {
        ulog("%s", err);
        return CONN_LOADER_FAILED;
    }
    ulog("Loader M-Flasher %s at %08lX, %u MHz", info->loader, (unsigned long)info->loader_addr, info->mhz);
    ulog("Chip key %04X, flash ID %06lX", info->chip_key, (unsigned long)info->flash_id);
    if (info->has_unique_id) {
        char id[17];
        for (int i = 0; i < 8; i++) snprintf(id + 2 * i, 3, "%02x", info->unique_id[i]);
        ulog("Unit ID %s", id);
    } else {
        ulog("No unit ID from the loader");
    }
    unit = identify_unit(*info);
    ulog("Unit: %s (%s)", unit.name, unit.why);
    if (info->retries) ulog("%lu transfers retried", (unsigned long)info->retries);
    return CONN_OK;
}

ConnectResult connect(jl::Info *info, char *err, size_t errlen) {
    usbkey::bus_float();
    other = {};
    if (!host::start()) {
        snprintf(err, errlen, "could not start the USB host");
        ulog("%s", err);
        return CONN_HOST_ERROR;
    }
    uint16_t vid = 0, pid = 0;
    if (!host::wait_device(12000, &vid, &pid, ulog, err, errlen)) {
        ulog("%s", err);
        if (vid) {
            ulog("Device %04X:%04X: not the download mode", vid, pid);
            inspect_other(vid, pid);
            return CONN_NOT_ROM;
        }
        return CONN_NO_DEVICE;
    }
    ulog("USB device %04X:%04X", vid, pid);
    if (!is_rom(vid, pid)) {
        snprintf(err, errlen, "%04X:%04X is not the download mode", vid, pid);
        ulog("%s", err);
        inspect_other(vid, pid);
        return CONN_NOT_ROM;
    }
    return start_loader(info, err, errlen);
}

ConnectResult soft_key(jl::Info *info, bool (*tick)(), char *err, size_t errlen) {
    if (!other.vid || other.midi != usbmidi::FOUND) {
        snprintf(err, errlen, "%04X:%04X is not USB MIDI with a bulk OUT: nothing sent", other.vid, other.pid);
        ulog("%s", err);
        return CONN_NOT_MIDI;
    }
    uint8_t pkt[8];
    size_t n = usbmidi::sysex_packets(usbmidi::SOFT_KEY, sizeof usbmidi::SOFT_KEY, 0, pkt, sizeof pkt);
    ulog("Soft key (M-Flasher) to %04X:%04X \"%s\"", other.vid, other.pid, other.product);
    ulog("Interface %u, EP %02X, %u-byte packets, cable 0", other.out.iface, other.out.ep, other.out.mps);
    ulog("Sending %u bytes: %02X %02X %02X %02X %02X %02X %02X %02X", (unsigned)n, pkt[0], pkt[1], pkt[2], pkt[3],
         pkt[4], pkt[5], pkt[6], pkt[7]);
    uint32_t sent = 0;
    int st = -1;
    uint32_t t0 = millis();
    bool ok = host::bulk_out(other.out.iface, other.out.alt, other.out.ep, pkt, n, &sent, &st, err, errlen);
    ulog("Transfer: %s, %lu of %u bytes", host::status_name(st), (unsigned long)sent, (unsigned)n);
    if (!ok) {
        ulog("%s", err);
        // a unit that restarts at once may leave before the transfer ends
        if (host::connected()) return CONN_SEND_FAILED;
        ulog("The device left during the transfer.");
    }

    ulog("Waiting up to %lu s for it to restart", (unsigned long)(SOFT_KEY_MS / 1000));
    ulog("into the download mode (4C4A:8057).");
    if (host::connected() && !host::wait_gone(SOFT_KEY_MS, tick)) {
        if (millis() - t0 < SOFT_KEY_MS) {
            snprintf(err, errlen, "cancelled while waiting");
            ulog("Cancelled: it was still connected.");
            return CONN_CANCELLED;
        }
        snprintf(err, errlen, "the firmware did not answer the MIDI request (it may not support it)");
        ulog("Still connected after %lu s:", (unsigned long)(SOFT_KEY_MS / 1000));
        ulog("%s", err);
        return CONN_NO_ANSWER;
    }
    ulog("Disconnect seen after %lu ms", (unsigned long)(millis() - t0));
    host::close_device(ulog);

    for (;;) {
        uint32_t used = millis() - t0;
        if (used >= SOFT_KEY_MS) {
            snprintf(err, errlen, "it left USB, but no download mode in %lu s", (unsigned long)(SOFT_KEY_MS / 1000));
            ulog("%s", err);
            return CONN_NOT_BACK;
        }
        uint16_t vid = 0, pid = 0;
        bool opened = host::wait_device(SOFT_KEY_MS - used, &vid, &pid, ulog, err, errlen);
        if (!vid) {
            ulog("%s", err);
            snprintf(err, errlen, "it left USB and did not come back in %lu s", (unsigned long)(SOFT_KEY_MS / 1000));
            ulog("%s", err);
            return CONN_NOT_BACK;
        }
        host::Device d;
        bool named = host::device(&d) && d.product[0];
        ulog("Reconnected after %lu ms: %04X:%04X%s%s%s", (unsigned long)(millis() - t0), vid, pid,
             named ? " \"" : "", named ? d.product : "", named ? "\"" : "");
        if (is_rom(vid, pid)) {
            if (!opened) {
                ulog("%s", err);
                return CONN_NOT_BACK;
            }
            ulog("USB device %04X:%04X: the download mode", vid, pid);
            return start_loader(info, err, errlen);
        }
        // not the download mode (yet): it may still be on its way there
        ulog("Not the download mode; waiting for it to leave again.");
        used = millis() - t0;
        if (used >= SOFT_KEY_MS || !host::wait_gone(SOFT_KEY_MS - used, tick)) {
            if (millis() - t0 < SOFT_KEY_MS) {
                snprintf(err, errlen, "cancelled while waiting");
                ulog("Cancelled: %04X:%04X was connected.", vid, pid);
                return CONN_CANCELLED;
            }
            snprintf(err, errlen, "it came back as %04X:%04X, not the download mode", vid, pid);
            ulog("%s", err);
            ulog("The firmware did not answer the MIDI request (it may not support it).");
            return CONN_NOT_BACK;
        }
        ulog("Disconnect seen after %lu ms", (unsigned long)(millis() - t0));
        host::close_device(ulog);
    }
}

bool keepalive() { return jl::ping() || host::connected(); }

// ---------------------------------------------------------------------------
// Backup

static const char *model_hint(uint32_t jedec, bool trimdat, bool trimcopy) {
    if (trimdat || trimcopy) return "SMK-37 (calibration record)";
    if (jedec == 0x856014 || jedec == 0xB36014) return "FM-1 (by flash ID)";
    if (jedec == 0xEB6014) return "SMK-37 (by flash ID)";
    return "unknown";
}

// Reads the whole flash into path; returns its CRC32 (and the model marks).
static bool read_to_file(const char *path, uint32_t *crc, bool *trimdat, bool *trimcopy, Progress progress,
                         uint32_t pass) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        ulog("Cannot create %s", path);
        return false;
    }
    char err[96];
    *crc = 0;
    uint32_t t0 = millis();
    for (uint32_t a = 0; a < FLASH_SIZE; a += CHUNK) {
        if (!jl::read(a, chunk_buf, CHUNK, err, sizeof err)) {
            f.close();
            ulog("Read failed: %s", err);
            return false;
        }
        if (f.write(chunk_buf, CHUNK) != CHUNK) {
            f.close();
            ulog("SD write failed at 0x%05lX", (unsigned long)a);
            return false;
        }
        *crc = jl::crc32_update(*crc, chunk_buf, CHUNK);
        if (a <= 0xC1224 && 0xC1224 + 8 <= a + CHUNK) *trimdat = memcmp(chunk_buf + (0xC1224 - a), "trimdat\0", 8) == 0;
        if (a <= 0xFF000 && 0xFF000 + 8 <= a + CHUNK) *trimcopy = memcmp(chunk_buf + (0xFF000 - a), "TRIMCOPY", 8) == 0;
        // reading is most of the work; a second pass (rare) starts the bar again
        if (progress) progress(a + CHUNK, FLASH_SIZE + FLASH_SIZE / 8);
    }
    f.close();
    ulog("Read 1 MiB in %lu ms (pass %lu)", (unsigned long)(millis() - t0), (unsigned long)pass);
    return true;
}

bool file_crc(const char *path, uint32_t *crc) {
    File f = SD.open(path, FILE_READ);
    if (!f || f.size() != FLASH_SIZE) return false;
    *crc = 0;
    for (uint32_t a = 0; a < FLASH_SIZE; a += CHUNK) {
        if (f.read(chunk_buf, CHUNK) != (int)CHUNK) {
            f.close();
            return false;
        }
        *crc = jl::crc32_update(*crc, chunk_buf, CHUNK);
    }
    f.close();
    return true;
}

bool read_flash(const char *path, uint32_t *out_crc, bool *smk, Progress progress) {
    char err[96];
    uint32_t chip_crc;
    if (!jl::crc32(0, FLASH_SIZE, &chip_crc, err, sizeof err)) {
        ulog("Chip CRC failed: %s", err);
        return false;
    }
    bool trimdat = false, trimcopy = false;
    uint32_t crc = 0;
    bool ok = false;
    for (uint32_t pass = 1; pass <= 2 && !ok; pass++) {
        if (!read_to_file(path, &crc, &trimdat, &trimcopy, progress, pass)) return false;
        ok = crc == chip_crc;
        if (!ok) ulog("Pass %lu: CRC %08lX, chip says %08lX", (unsigned long)pass, (unsigned long)crc, (unsigned long)chip_crc);
    }
    if (!ok) {
        ulog("The reads do not match the chip: backup NOT valid.");
        return false;
    }
    uint32_t back;
    if (!file_crc(path, &back) || back != chip_crc) {
        ulog("Read back from SD does not match: NOT valid.");
        return false;
    }
    if (progress) progress(1, 1);
    if (out_crc) *out_crc = chip_crc;
    if (smk) *smk = trimdat || trimcopy;
    return true;
}

bool backup(const jl::Info &info, char *out_path, size_t pathlen, bool *smk, Progress progress) {
    if (!sd_ready()) {
        ulog("No SD card: insert one (FAT32), try again.");
        return false;
    }
    char base[40], path[48];
    for (int n = 1; n < 1000; n++) {
        snprintf(base, sizeof base, "/fm1/backup-%03d", n);
        snprintf(path, sizeof path, "%s.bin", base);
        if (!SD.exists(path)) break;
    }
    ulog("Backing up to %s", path);
    uint32_t chip_crc;
    bool marks = false;
    if (!read_flash(path, &chip_crc, &marks, progress)) return false;
    if (out_path) snprintf(out_path, pathlen, "%s", path);
    if (smk) *smk = marks;
    // the model as the calibration marks show it (read_flash saw them)
    bool trimdat = marks, trimcopy = false;
    snprintf(path, sizeof path, "%s.txt", base);
    File f = SD.open(path, FILE_WRITE);
    if (f) {
        f.printf("unit: %s\n", model_hint(info.flash_id, trimdat, trimcopy));
        f.printf("chip key: %04X\nflash id: %06lX\n", info.chip_key, (unsigned long)info.flash_id);
        if (info.has_unique_id) {
            f.print("unit id: ");
            for (int i = 0; i < 8; i++) f.printf("%02x", info.unique_id[i]);
            f.print("\n");
        }
        f.printf("loader: M-Flasher %s, CPU %u MHz\n", info.loader, info.mhz);
        f.printf("size: %lu\ncrc32: %08lx\nretries: %lu\n", (unsigned long)FLASH_SIZE, (unsigned long)chip_crc,
                 (unsigned long)info.retries);
        f.close();
    }
    ulog("OK: CRC32 %08lX matches the chip.", (unsigned long)chip_crc);
    ulog("Unit: %s", model_hint(info.flash_id, trimdat, trimcopy));
    return true;
}

// ---------------------------------------------------------------------------
// Restore: the backups on the card

int list_backups(int *nums, int max) {
    if (!sd_ready()) return 0;
    File dir = SD.open("/fm1");
    if (!dir) return 0;
    int count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char *name = strrchr(f.name(), '/');
        name = name ? name + 1 : f.name();
        // exactly backup-NNN.bin
        bool ok = !f.isDirectory() && strlen(name) == 14 && !strncmp(name, "backup-", 7) && isdigit(name[7]) &&
                  isdigit(name[8]) && isdigit(name[9]) && !strcmp(name + 10, ".bin");
        int n = ok ? atoi(name + 7) : 0;
        f.close();
        if (n >= 1 && count < max) nums[count++] = n;
    }
    dir.close();
    for (int i = 1; i < count; i++)  // newest (highest) first
        for (int j = i; j > 0 && nums[j] > nums[j - 1]; j--) {
            int t = nums[j];
            nums[j] = nums[j - 1];
            nums[j - 1] = t;
        }
    return count;
}

bool backup_notes(int n, restore::Notes *notes) {
    char path[40], text[512];
    snprintf(path, sizeof path, "/fm1/backup-%03d.txt", n);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    int len = f.read((uint8_t *)text, sizeof text - 1);
    f.close();
    if (len <= 0) return false;
    text[len] = 0;
    restore::parse_notes(text, notes);
    return true;
}

static File restore_file;

static bool read_backup(void *, uint32_t s, uint8_t *buf, char *err, size_t errlen) {
    uint32_t a = s * writer::SECTOR;
    if (!restore_file || !restore_file.seek(a) || restore_file.read(buf, writer::SECTOR) != writer::SECTOR) {
        snprintf(err, errlen, "SD card: cannot read the backup at 0x%05lX", (unsigned long)a);
        return false;
    }
    return true;
}

bool open_file(const char *path, writer::Source *src, uint32_t *size) {
    close_backup();
    if (!sd_ready()) return false;
    restore_file = SD.open(path, FILE_READ);
    if (!restore_file) return false;
    *size = restore_file.size();
    src->read = read_backup;
    src->ctx = nullptr;
    return true;
}

bool open_backup(int n, writer::Source *src, uint32_t *size) {
    char path[40];
    snprintf(path, sizeof path, "/fm1/backup-%03d.bin", n);
    return open_file(path, src, size);
}

void close_backup() {
    if (restore_file) restore_file.close();
}

}  // namespace engine
