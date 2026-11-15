// The firmware manager's library (docs/FIRMWARE-MANAGER.md, phase 2): names,
// files and metadata of a unit's snapshots on the SD card. Pure logic, no SD
// card and no screen here (src/library_sd.cpp does the files), so it builds
// and is tested on a PC (tests/host/test_library.cpp).
//
//   /fm1/units/<unit id>/unit.txt            model, chip key, flash ID, label
//   /fm1/units/<unit id>/current.txt         the snapshot last saved or loaded
//   /fm1/units/<unit id>/snapshots/NNNN <safe name>.bin / .txt   "(no folder)"
//   /fm1/units/<unit id>/snapshots/<folder>/NNNN <safe name>.bin / .txt
//   /fm1/backup-NNN.bin / .txt               older backups: listed, importable
//
// Folders are one level deep, named by the user (FAT-safe). The numbers are
// unique across all of a unit's folders, so a snapshot keeps its number when
// it is moved from one folder to another.
//
// A snapshot's .txt is a backup's .txt (restore::parse_notes reads it: chip
// key, flash id, unit id, size, crc32) with more lines: name (as typed),
// date, model, firmware, source.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "jl.h"
#include "restore.h"

namespace library {

static constexpr size_t NAME_CHARS = 36;       // a typed name, characters
static constexpr size_t SAFE_MAX = 32;       // its FAT-safe copy in the file name
static constexpr size_t BASE_LEN = 4 + 1 + SAFE_MAX + 1;  // "NNNN safe" and its 0
static constexpr int NUMBER_MAX = 9999;
static constexpr size_t FOLDER_CHARS = 24;    // a folder's name (FAT-safe as typed)
static constexpr int MAX_FOLDERS = 24;        // folders of a unit that are listed

// --- names

// A typed name cleaned up: control characters dropped, spaces trimmed and
// runs of them made one, at most NAME_CHARS characters.
void clean_name(const char *in, char *out, size_t outlen);
// The FAT-safe copy of a name: / \ : * ? " < > | and control characters
// become -, spaces trimmed, no trailing dot, at most SAFE_MAX characters;
// "snapshot" if nothing is left.
void safe_name(const char *name, char *out, size_t outlen);
// "NNNN safe name": the file name without .bin / .txt.
void base_name(int number, const char *name, char *out, size_t outlen);
// "NNNN anything.bin" (or .txt with ext): its number and base; false otherwise.
bool parse_snapshot_file(const char *file, const char *ext, int *number, char *base, size_t baselen);
// "backup-NNN.bin": its number; false otherwise.
bool parse_legacy_file(const char *file, int *number);

// The name window's suggestion: "<firmware> <date>" when both are known,
// else what is known ("<model> <date>" with only a date), else the model
// ("SMK-37"), else "snapshot". firmware: the USB product name seen before
// the soft key (trimmed here), or "".
void suggest(const char *firmware, const char *date, const char *model, char *out, size_t outlen);

// --- folders

// A typed folder name as the directory's name: cleaned as a name, / \ : * ?
// " < > | and anything not ASCII become -, leading dots and spaces dropped,
// no trailing dot or space, at most FOLDER_CHARS characters. False when
// nothing is left (out is then "").
bool folder_name(const char *typed, char *out, size_t outlen);
// The same folder on a FAT card: names equal but for case ("" is "(no folder)").
bool same_folder(const char *a, const char *b);
// <unit_dir>/snapshots[/<folder>]/<base><ext>
void snapshot_path(const char *unit_dir, const char *folder, const char *base, const char *ext, char *out,
                   size_t outlen);

// --- a snapshot's .txt

enum Source { SRC_UNKNOWN, SRC_BACKUP, SRC_AUTO, SRC_IMPORT };
const char *source_name(Source s);  // "backup", "auto-save", "import", ""

struct Meta {
    char name[NAME_CHARS + 1];  // as typed; "" for an old backup's .txt
    char date[16];            // "" when not known (no clock)
    char model[16];           // "SMK-37", "FM-1"; from an old .txt's "unit:" line
    char firmware[48];        // the firmware hint, or ""
    char loader[48];          // "M-Flasher 0.4, CPU 192 MHz", or ""
    char imported[24];        // an import: where from ("backup-012"), or ""
    Source source;
    restore::Notes notes;     // chip key, flash id, unit id, size, crc32
};
// Writes the .txt; false if it does not fit in buf.
bool format_meta(const Meta &m, char *buf, size_t buflen);
// Reads one (a snapshot's or an old backup's).
void parse_meta(const char *text, Meta *m);
// A Meta for the connected unit's flash (crc, size 1 MiB).
void meta_for(const jl::Info &info, const char *name, const char *date, const char *model, const char *firmware,
              Source source, uint32_t crc, Meta *m);

// --- the unit

// The unit's folder name: its flash unique ID in hex (as "unit id" in a
// .txt), else "k<chip key>-f<CRC32 of the factory data>" (factory_hash).
void unit_id(const jl::Info &info, uint32_t factory_hash, char *out, size_t outlen);
// CRC32 of the factory data the restore compares (restore.h): USRTRIM
// (0xC1000-0xC1FFF; nullptr on an FM-1) and key_mac (0xFF23C-0xFFFFF, in
// last_sector: 0xFF000-0xFFFFF).
uint32_t factory_hash(const uint8_t *usrtrim, const uint8_t *last_sector);

struct UnitFile {
    char model[16];
    uint16_t chip_key;
    uint32_t flash_id;
    char first_seen[16];  // a date, or "" (no clock)
    char label[NAME_CHARS + 1];
};
bool format_unit(const UnitFile &u, char *buf, size_t buflen);
void parse_unit(const char *text, UnitFile *u);

// current.txt: the number of the snapshot last saved or loaded (and its
// name, for a person reading the card). 0: none.
bool format_current(int number, const char *name, char *buf, size_t buflen);
int parse_current(const char *text);

// --- the list

enum Kind { SNAPSHOT, LEGACY };
enum Whose { WHOSE_THIS, WHOSE_OTHER, WHOSE_NO_ID, WHOSE_NO_TXT };
struct Entry {
    Kind kind;
    int number;                   // NNNN of a snapshot, NNN of backup-NNN
    char base[BASE_LEN + 8];      // the file name without .bin
    char folder[FOLDER_CHARS + 1];  // a snapshot's folder; "": none (the snapshots folder itself)
    char name[NAME_CHARS + 1];      // as shown
    char date[16];                // from its .txt; "" when not known
    bool has_crc;
    uint32_t crc;
    Source source;
    char model[16];
    bool current;
    Whose whose;                  // a legacy backup's
};
// Fills a snapshot's entry from its file name and .txt (text nullptr: none),
// in folder ("" or nullptr: none).
void snapshot_entry(int number, const char *base, const char *text, Entry *e, const char *folder = "");
// Fills an old backup's entry from its .txt, for the connected unit.
void legacy_entry(int number, const char *text, const jl::Info &info, Entry *e);
Whose whose(const restore::Notes *notes, const jl::Info &info);
const char *whose_name(Whose w);  // "this unit", "OTHER unit", ...

// Newest first: snapshots by number (highest first), then old backups (the
// same); marks the current one.
void sort(Entry *e, int n, int current);
// The number for a new snapshot: one more than the highest there is (never
// one that was deleted and might still be in a log), 1 for the first; 0 if
// NUMBER_MAX is used up.
int next_number(const Entry *e, int n);
// A snapshot with this CRC32 (its .txt's), in any folder, or -1.
int find_crc(const Entry *e, int n, uint32_t crc);
int count_snapshots(const Entry *e, int n);

// "<folder> / <name>", or the name alone for "(no folder)".
void label(const Entry &e, char *out, size_t outlen);

// What the device holds is saved: the snapshot (any folder) whose CRC32 is
// the chip's, skipping the indices in skip (their files failed the check
// that they really hold that CRC32), or -1: not saved. LOAD asks "save
// first?" only when it is -1.
int saved_as(const Entry *e, int n, uint32_t chip_crc, const int *skip, int nskip);

// A unit's folders as the screens list them: [0] is "(no folder)" (name ""),
// then the folders found on the card (dirs), in alphabetical order (case
// ignored), each with its snapshots counted and its newest (the highest
// number; an index into e, -1 when empty). How many, at most max.
struct Folder {
    char name[FOLDER_CHARS + 1];
    int count;
    int newest;
};
int folders(const Entry *e, int n, const char (*dirs)[FOLDER_CHARS + 1], int ndirs, Folder *out, int max);
// The snapshots in one folder, oldest first (by number): their indices into
// e, at most max; how many.
int in_folder(const Entry *e, int n, const char *folder, int *idx, int max);

// The connected unit's model and the snapshot's: equal, or one is unknown.
bool same_model(const char *unit_model, const char *snap_model);

// --- the Cardputer's battery before a write
enum Battery { BATTERY_OK, BATTERY_LOW, BATTERY_UNKNOWN };
static constexpr int BATTERY_MIN_PERCENT = 20;
// mv: the battery voltage read (M5Unified); a reading out of 2500-4500 mV is
// not believed (UNKNOWN: no refusal). percent gets the estimate
// (3300 mV 0 %, 4100 mV 100 %, M5Unified's scale).
Battery battery(int mv, int *percent);

}  // namespace library
