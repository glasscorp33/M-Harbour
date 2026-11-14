// What both modes do with a unit, without any screen: download mode (key and
// calibration), the USB host and the loader, which unit it is, the backup.
// Everything is logged with ulog(); progress comes back through callbacks.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "jl.h"
#include "restore.h"
#include "usb_midi.h"
#include "usbkey.h"
#include "writer.h"

namespace engine {

static constexpr uint32_t FLASH_SIZE = 0x100000;
typedef void (*Progress)(uint32_t done, uint32_t total);

// --- download mode
enum EnterResult { ENTER_OK, ENTER_ALREADY_ON, ENTER_TIMEOUT, ENTER_CANCELLED, ENTER_FAILED };
struct EnterHooks {
    bool (*poll)(const usbkey::KeyStats *);  // while sending the key: true cancels
    void (*answered)(bool ack);               // the ROM answered: calibration starts
    bool (*cancel)();                         // during calibration: true cancels
};
EnterResult enter_download_mode(const EnterHooks &h, uint32_t timeout_ms, usbkey::KeyStats *ks);

// --- the USB host and the loader
enum ConnectResult {
    CONN_OK,
    CONN_HOST_ERROR,
    CONN_NO_DEVICE,
    CONN_NOT_ROM,        // another device: see `other`
    CONN_LOADER_FAILED,
    // soft_key() only
    CONN_NOT_MIDI,       // not a USB MIDI device with a bulk OUT: nothing sent
    CONN_SEND_FAILED,    // the MIDI transfer failed, the device is still there
    CONN_NO_ANSWER,      // sent, but it did not restart: the firmware may not know it
    CONN_NOT_BACK,       // it left USB, but did not come back as the download mode
    CONN_CANCELLED,
};
ConnectResult connect(jl::Info *info, char *err, size_t errlen);

// After CONN_NOT_ROM: the device that answered instead of the download mode
// (a unit running its firmware, m-bootloader's MIDI mode, a bootloader's
// update loader, ...).
struct Other {
    uint16_t vid, pid;
    char product[48];
    usbmidi::Find midi;   // FOUND: `out` is its first MIDI OUT
    usbmidi::MidiOut out;
};
extern Other other;

// --- a running unit: the soft key over USB MIDI (M-Flasher's src/midi.rs)
// After CONN_NOT_ROM with other.midi == FOUND, and only when the user asked
// for it: sends F0 22 24 35 7D F7 to the first MIDI OUT (cable 0), waits up
// to SOFT_KEY_MS for the unit to leave USB and come back as the download mode
// (4C4A:8057), then goes on as connect() does. Nothing else is ever sent to
// the device. tick: called while waiting, true cancels.
static constexpr uint32_t SOFT_KEY_MS = 30000;
ConnectResult soft_key(jl::Info *info, bool (*tick)(), char *err, size_t errlen);

// --- which unit: decided before anything can be written
enum Model { MODEL_UNKNOWN, MODEL_FM1, MODEL_SMK37 };
struct Unit {
    Model model;
    const char *name, *why;
};
extern Unit unit;  // set by connect()

// --- a full backup to the SD card, checked against the chip's CRC32 and read
// back from the card. path gets its path; smk: SMK-37 calibration marks seen.
bool backup(const jl::Info &info, char *path, size_t pathlen, bool *smk, Progress progress);

// --- restoring a backup from the SD card
// The backups' numbers (/fm1/backup-NNN.bin), newest first: how many, at most max.
int list_backups(int *nums, int max);
// Backup n's .txt; false without one.
bool backup_notes(int n, restore::Notes *notes);
// Opens backup n to be read through src (restore::check, the writer), a
// sector at a time, until close_backup(); size gets the file's size.
bool open_backup(int n, writer::Source *src, uint32_t *size);
void close_backup();

// Keeps the session busy; false when the unit is gone.
bool keepalive();

}  // namespace engine
