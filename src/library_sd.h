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

// The unit's snapshots (all its folders) and every old backup, newest
// first, current marked.
int list(const Unit &u, const jl::Info &info, library::Entry *e, int max);
// The unit's folders on the card (the directories in snapshots/), at most
// library::MAX_FOLDERS; how many.
int folder_dirs(const Unit &u, char (*dirs)[library::FOLDER_CHARS + 1], int max);
int current(const Unit &u);
bool set_current(const Unit &u, int number, const char *name);

// Paths of an entry's files.
void bin_path(const Unit &u, const library::Entry &e, char *out, size_t n);
void txt_path(const Unit &u, const library::Entry &e, char *out, size_t n);
// An entry's .txt; false without one.
bool read_meta(const Unit &u, const library::Entry &e, library::Meta *m);

// SAVE: the unit's whole flash as a new snapshot (verified as a backup is),
// and its .txt, in folder ("": none; it must exist). number gets its
// number, crc its CRC32 (may be nullptr).
bool save(Unit *u, const jl::Info &info, const char *folder, const char *name, const char *firmware,
          library::Source source, int *number, uint32_t *crc, engine::Progress progress);
// Whether e's .bin really holds the CRC32 its .txt says (the file read).
bool file_matches(const Unit &u, const library::Entry &e);
// What the device holds is saved (library::saved_as), its file checked:
// the index into e of the snapshot that holds chip_crc, or -1.
int saved_as(const Unit &u, const library::Entry *e, int n, uint32_t chip_crc);
// IMPORT: copies an old backup (checked by the caller with restore::check)
// into the library with a name ("(no folder)"); the copy's CRC32 must be the backup's.
bool import(Unit *u, const jl::Info &info, const library::Entry &legacy, const char *name, int *number,
            engine::Progress progress);
bool rename(const Unit &u, const library::Entry &e, const char *name);
bool remove(const Unit &u, const library::Entry &e);
// MOVE: both files into another folder ("": none), the same names.
bool move(const Unit &u, const library::Entry &e, const char *to_folder);

// Folders. made gets the folder's name as on the card (library::folder_name).
// False (and why, for the screen) if the name is empty, taken, or the card fails.
bool make_folder(Unit *u, const jl::Info &info, const char *typed, char *made, size_t madelen, char *why,
                 size_t whylen);
bool rename_folder(const Unit &u, const char *from, const char *typed, char *made, size_t madelen, char *why,
                   size_t whylen);
// Only an empty folder (no file at all in it); false with why otherwise.
bool remove_folder(const Unit &u, const char *name, char *why, size_t whylen);

}  // namespace libsd
