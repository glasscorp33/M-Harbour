// The firmware manager's switch (LOAD, docs/FIRMWARE-MANAGER.md section 5):
// a snapshot of the library back onto its unit. No screen and no SD card
// here: the steps that need them come through Hooks, so the order of things
// is tested on a PC (tests/host/test_library.cpp) on the fake unit.
//
// Saving what the unit holds first is the caller's: the firmware manager
// asks "save first?" when it is not in the library (library::saved_as), so
// there is no auto-save in here.
//
//  1. the Cardputer's battery (hook; low: nothing done)
//  2. the restore's checks on the snapshot (whole, two reads, CRC32, this
//     unit by ID and factory data) and the model; refused: nothing written
//  3. the plan from 0x4000 (the unit's boot area kept); nothing differs and
//     the boot areas are equal: nothing to do
//  4. confirm (hook): write, cancel, or "restore exactly" when the boot
//     areas differ (then the plan from 0, confirmed again)
//  5. the battery again, then the writer (never the protected ranges)
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "jl.h"
#include "restore.h"
#include "writer.h"

namespace switcher {

enum Choice { CANCEL, WRITE, EXACT };
enum Outcome {
    CANCELLED,        // nothing written
    BATTERY_LOW,      // nothing written (err: why)
    REFUSED,          // the checks said no (report.check.why, err)
    READ_FAILED,      // the unit or the card could not be read: nothing written
    NOTHING_TO_DO,    // the unit already holds the snapshot
    WRITTEN,
    WRITE_FAILED,     // stopped part-way (err); the same load can be run again
};
const char *outcome_name(Outcome o);

struct Request {
    const jl::Info *info;
    bool smk37;                  // the connected unit is an SMK-37 (else an FM-1)
    const char *unit_model;      // "SMK-37", "FM-1"
    const char *snap_model;      // its .txt's model, or ""
    uint32_t size;               // the snapshot file's size
    const restore::Notes *notes; // its .txt; nullptr: none (refused)
    writer::Source *src;         // the file
    const char *name;            // for the log and the writer
};

struct Report {
    restore::Result check;
    uint32_t unit_boot_crc;      // the unit's 0x0-0x3FFF now
    bool boot_differs;           // ... is not the snapshot's
    uint32_t start;              // what is being written from: BOOT_AREA, or 0 (exactly)
    writer::Plan plan;
};

struct Hooks {
    void *ctx;
    // false: too low to write (why says so). nullptr: not checked.
    bool (*battery)(void *ctx, char *why, size_t n);
    // stage 0: the plan with the boot area kept (EXACT counts only if
    // boot_differs); stage 1: the exact plan (WRITE confirms it).
    Choice (*confirm)(void *ctx, int stage, const Report &r);
    writer::Log log;
    writer::Progress progress;   // checking, comparing, writing
};

Outcome load(const Request &req, const Hooks &h, Report *rep, char *err, size_t errlen);

}  // namespace switcher
