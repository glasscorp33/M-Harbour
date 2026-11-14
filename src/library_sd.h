// The library's files on the SD card (src/library.h has the layout and the
// rules; this file only reads and writes them). Everything is logged.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "engine.h"
#include "jl.h"
#include "library.h"
#include "writer.h"

namespace libsd {

static constexpr int MAX_ENTRIES = 64;  // snapshots and old backups listed (more are not shown)

struct Unit {
    char id[24];               // the folder's name
    char dir[48];              // /fm1/units/<id>
    bool known;                // unit.txt was there
    library::UnitFile file;
    const char *model;         // "SMK-37", "FM-1" (the connected unit's)
};

// The date for names and .txt files. The Cardputer ADV has no clock: "".
// TODO(phase 2+): Wi-Fi time (NTP) when /fm1/wifi.txt holds a network.
const char *today();

// The connected unit's folder (its unique ID; without one, reads the factory
// data to make the ID). False: the unit could not be read.
bool identify(const jl::Info &info, bool smk37, const char *model, Unit *u, char *err, size_t errlen);
// Makes the folders and unit.txt if they are not there yet.
bool ensure(Unit *u, const jl::Info &info);
bool set_label(Unit *u, const jl::Info &info, const char *label);

// The unit's snapshots and every old backup, newest first, current marked.
int list(const Unit &u, const jl::Info &info, library::Entry *e, int max);
int current(const Unit &u);
bool set_current(const Unit &u, int number, const char *name);

// Paths of an entry's files.
void bin_path(const Unit &u, const library::Entry &e, char *out, size_t n);
void txt_path(const Unit &u, const library::Entry &e, char *out, size_t n);
// An entry's .txt; false without one.
bool read_meta(const Unit &u, const library::Entry &e, library::Meta *m);

// SAVE: the unit's whole flash as a new snapshot (verified as a backup is),
// and its .txt. number gets its number.
bool save(Unit *u, const jl::Info &info, const char *name, const char *firmware, library::Source source, int *number,
          engine::Progress progress);
// The auto-save before a LOAD: unless the flash equals a snapshot already
// there (its CRC32, and its file checked), saved as "auto <current>".
// what: what happened, for the screen ("equals 0003 Dub 1", "0005 auto ...").
bool auto_save(Unit *u, const jl::Info &info, const char *firmware, char *what, size_t whatlen,
               engine::Progress progress);
// IMPORT: copies an old backup (checked by the caller with restore::check)
// into the library with a name; the copy's CRC32 must be the backup's.
bool import(Unit *u, const jl::Info &info, const library::Entry &legacy, const char *name, int *number,
            engine::Progress progress);
bool rename(const Unit &u, const library::Entry &e, const char *name);
bool remove(const Unit &u, const library::Entry &e);

}  // namespace libsd
