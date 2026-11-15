// Host tests for the firmware manager's library (src/library.cpp: names,
// files, metadata, the list) and the switch (src/switcher.cpp, on the fake
// unit of fake_jl.cpp). Plain C++, no framework: tests/host/run.sh.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fake_jl.h"
#include "library.h"
#include "restore.h"
#include "switcher.h"
#include "writer.h"

static int failures, checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            failures++;                                                  \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                \
    } while (0)
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        checks++;                                                                \
        if (strcmp((a), (b))) {                                                  \
            failures++;                                                          \
            printf("  FAILED %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                        \
    } while (0)

static constexpr uint32_t SIZE = 0x100000, SECTOR = 0x1000;
static const uint8_t UID_A[8] = {0x41, 0x50, 0x35, 0x33, 0x36, 0x32, 0x37, 0x15};
static const uint8_t UID_B[8] = {0x41, 0x50, 0x35, 0x33, 0x36, 0x32, 0x37, 0x16};

static jl::Info unit_info(const uint8_t *uid, uint32_t flash_id = 0x856014) {
    jl::Info i;
    memset(&i, 0, sizeof i);
    i.chip_key = 0x980F;
    i.flash_id = flash_id;
    snprintf(i.loader, sizeof i.loader, "0.4");
    i.mhz = 192;
    if (uid) {
        memcpy(i.unique_id, uid, 8);
        i.has_unique_id = true;
    }
    return i;
}

// ---------------------------------------------------------------------------
// Names

static void test_names() {
    puts("library: typed names cleaned, FAT-safe file names");
    char out[64];
    library::clean_name("  Dub   1 \t\n ", out, sizeof out);
    CHECK_STR(out, "Dub 1");
    library::clean_name("", out, sizeof out);
    CHECK_STR(out, "");
    library::clean_name("abcdefghijklmnopqrstuvwxyz0123456789XYZ", out, sizeof out);
    CHECK(strlen(out) == library::NAME_CHARS);
    library::clean_name("abc", out, 3);  // a small buffer
    CHECK_STR(out, "ab");

    library::safe_name("FMini-1 16/09/26", out, sizeof out);
    CHECK_STR(out, "FMini-1 16-09-26");
    library::safe_name("a/b\\c:d*e?f\"g<h>i|j", out, sizeof out);
    CHECK_STR(out, "a-b-c-d-e-f-g-h-i-j");
    library::safe_name("  Hip hop 2.  ", out, sizeof out);
    CHECK_STR(out, "Hip hop 2");
    library::safe_name("...", out, sizeof out);
    CHECK_STR(out, "snapshot");
    library::safe_name("   ", out, sizeof out);
    CHECK_STR(out, "snapshot");
    library::safe_name("\x01\x02", out, sizeof out);
    CHECK_STR(out, "snapshot");
    library::safe_name("caf\xc3\xa9", out, sizeof out);  // not ASCII: - in the file name
    CHECK_STR(out, "caf--");
    library::safe_name("abcdefghijklmnopqrstuvwxyz0123456789XYZ", out, sizeof out);
    CHECK(strlen(out) == library::SAFE_MAX);
    library::safe_name("abcdefghijklmnopqrstuvwxyz01234 6789", out, sizeof out);  // cut at a space
    CHECK_STR(out, "abcdefghijklmnopqrstuvwxyz01234");

    library::base_name(1, "Blank start", out, sizeof out);
    CHECK_STR(out, "0001 Blank start");
    library::base_name(12, "SMK37 Open 14/11/26", out, sizeof out);
    CHECK_STR(out, "0012 SMK37 Open 14-11-26");
    char a[64], b[64];
    library::base_name(3, "Dub 1", a, sizeof a);
    library::base_name(4, "Dub 1", b, sizeof b);
    CHECK(strcmp(a, b) != 0);  // equal names, other files
    library::base_name(5, "Dub/1", a, sizeof a);
    library::base_name(6, "Dub:1", b, sizeof b);
    CHECK(strcmp(a, b) != 0);  // equal once made safe, other files

    int n;
    char base[64];
    CHECK(library::parse_snapshot_file("0003 Dub 1.bin", ".bin", &n, base, sizeof base) && n == 3);
    CHECK_STR(base, "0003 Dub 1");
    CHECK(library::parse_snapshot_file("0010 x.BIN", ".bin", &n, base, sizeof base) && n == 10);
    CHECK(!library::parse_snapshot_file("0003 Dub 1.txt", ".bin", &n, base, sizeof base));
    CHECK(!library::parse_snapshot_file("003 Dub.bin", ".bin", &n, base, sizeof base));
    CHECK(!library::parse_snapshot_file("0003Dub.bin", ".bin", &n, base, sizeof base));
    CHECK(!library::parse_snapshot_file("0000 zero.bin", ".bin", &n, base, sizeof base));
    CHECK(!library::parse_snapshot_file("0003 .bin", ".bin", &n, base, sizeof base));
    CHECK(!library::parse_snapshot_file("0003 abc.bin", ".bin", &n, base, 6));  // does not fit
    CHECK(library::parse_legacy_file("backup-012.bin", &n) && n == 12);
    CHECK(!library::parse_legacy_file("backup-012.txt", &n));
    CHECK(!library::parse_legacy_file("backup-12.bin", &n));
    CHECK(!library::parse_legacy_file("backup-000.bin", &n));
}

static void test_suggestion() {
    puts("library: the name window's suggestion");
    char out[64];
    library::suggest("SMK37 Open   ", "14/11/26", "SMK-37", out, sizeof out);
    CHECK_STR(out, "SMK37 Open 14/11/26");
    library::suggest("SMK37 Open   ", "", "SMK-37", out, sizeof out);
    CHECK_STR(out, "SMK37 Open");
    library::suggest("", "14/11/26", "SMK-37", out, sizeof out);
    CHECK_STR(out, "SMK-37 14/11/26");
    library::suggest(nullptr, nullptr, "SMK-37", out, sizeof out);
    CHECK_STR(out, "SMK-37");
    library::suggest("", "", "", out, sizeof out);
    CHECK_STR(out, "snapshot");
    library::suggest("", "14/11/26", "", out, sizeof out);
    CHECK_STR(out, "14/11/26");
}

static void test_folder_names() {
    puts("library: folder names (FAT-safe, typed by the user), paths");
    char out[64];
    CHECK(library::folder_name("SeqAdv", out, sizeof out));
    CHECK_STR(out, "SeqAdv");
    CHECK(library::folder_name("  Official   Open ", out, sizeof out));
    CHECK_STR(out, "Official Open");
    CHECK(library::folder_name("Dub/Hip:hop*?", out, sizeof out));
    CHECK_STR(out, "Dub-Hip-hop--");
    CHECK(library::folder_name("a\\b\"c<d>e|f", out, sizeof out));
    CHECK_STR(out, "a-b-c-d-e-f");
    CHECK(library::folder_name("..hidden", out, sizeof out));  // no leading dots
    CHECK_STR(out, "hidden");
    CHECK(library::folder_name("v1.", out, sizeof out));  // FAT drops a trailing dot
    CHECK_STR(out, "v1");
    CHECK(library::folder_name("caf\xc3\xa9", out, sizeof out));
    CHECK_STR(out, "caf--");
    CHECK(!library::folder_name("", out, sizeof out) && !out[0]);
    CHECK(!library::folder_name("   ", out, sizeof out) && !out[0]);
    CHECK(!library::folder_name("...", out, sizeof out) && !out[0]);
    CHECK(!library::folder_name(". .", out, sizeof out) && !out[0]);
    CHECK(!library::folder_name(nullptr, out, sizeof out));
    CHECK(library::folder_name("abcdefghijklmnopqrstuvwxyz0123456789", out, sizeof out));
    CHECK(strlen(out) == library::FOLDER_CHARS);
    CHECK(library::folder_name("abcdefghijklmnopqrstuvw xyz", out, sizeof out));  // cut at the space
    CHECK_STR(out, "abcdefghijklmnopqrstuvw");
    CHECK(library::folder_name("abcdef", out, 4));  // a small buffer
    CHECK_STR(out, "abc");

    CHECK(library::same_folder("SeqAdv", "seqadv"));
    CHECK(library::same_folder("", ""));
    CHECK(library::same_folder(nullptr, ""));
    CHECK(!library::same_folder("SeqAdv", "SeqAdv2"));
    CHECK(!library::same_folder("SeqAdv", ""));

    library::snapshot_path("/fm1/units/41", "", "0003 Dub 1", ".bin", out, sizeof out);
    CHECK_STR(out, "/fm1/units/41/snapshots/0003 Dub 1.bin");
    library::snapshot_path("/fm1/units/41", nullptr, "0003 Dub 1", ".txt", out, sizeof out);
    CHECK_STR(out, "/fm1/units/41/snapshots/0003 Dub 1.txt");
    library::snapshot_path("/fm1/units/41", "SeqAdv", "0003 Dub 1", ".txt", out, sizeof out);
    CHECK_STR(out, "/fm1/units/41/snapshots/SeqAdv/0003 Dub 1.txt");
    // the longest: a unit ID, a folder and a name at their limits, in 128 bytes
    char longest[160];
    library::snapshot_path("/fm1/units/k980F-f12345678", "abcdefghijklmnopqrstuvwx",
                           "9999 abcdefghijklmnopqrstuvwxyz012345", ".bin", longest, sizeof longest);
    CHECK(strlen(longest) < 128);
}

static void test_meta() {
    puts("library: a snapshot's .txt, written and read back; the restore reads it too");
    jl::Info info = unit_info(UID_A);
    library::Meta m, back;
    library::meta_for(info, "Dub: 1 / \"new\"", "", "SMK-37", "SMK37 Open   ", library::SRC_BACKUP, 0x0A1B2C3D, &m);
    char txt[512];
    CHECK(library::format_meta(m, txt, sizeof txt));
    if (getenv("VERBOSE")) printf("%s", txt);
    CHECK(strstr(txt, "name: Dub: 1 / \"new\"\n") != nullptr);  // exactly as typed
    CHECK(strstr(txt, "date: \n") != nullptr);                  // not known: empty
    CHECK(strstr(txt, "unit id: 4150353336323715\n") != nullptr);
    CHECK(strstr(txt, "chip key: 980F\nflash id: 856014\n") != nullptr);
    library::parse_meta(txt, &back);
    CHECK_STR(back.name, "Dub: 1 / \"new\"");
    CHECK_STR(back.date, "");
    CHECK_STR(back.model, "SMK-37");
    CHECK_STR(back.firmware, "SMK37 Open");
    CHECK_STR(back.loader, "M-Flasher 0.4, CPU 192 MHz");
    CHECK(back.source == library::SRC_BACKUP);
    CHECK(back.notes.has_crc && back.notes.crc == 0x0A1B2C3D);
    CHECK(back.notes.has_size && back.notes.size == SIZE);
    CHECK(back.notes.has_id && !memcmp(back.notes.unique_id, UID_A, 8));
    // what restore::check is given
    restore::Notes n;
    restore::parse_notes(txt, &n);
    CHECK(n.has_key && n.chip_key == 0x980F && n.has_flash && n.flash_id == 0x856014);
    CHECK(n.has_id && n.has_crc && n.crc == 0x0A1B2C3D && n.has_size && n.size == SIZE);
    // a name that looks like a key does not fool either reader
    library::meta_for(info, "crc32: 12345678", "1/1/27", "FM-1", "", library::SRC_AUTO, 0xCAFEF00D, &m);
    CHECK(library::format_meta(m, txt, sizeof txt));
    restore::parse_notes(txt, &n);
    CHECK(n.crc == 0xCAFEF00D);
    library::parse_meta(txt, &back);
    CHECK_STR(back.name, "crc32: 12345678");
    CHECK_STR(back.date, "1/1/27");
    CHECK(back.source == library::SRC_AUTO);
    // an import says where from; no unit ID when the loader gave none
    library::meta_for(unit_info(nullptr), "old", "", "FM-1", "", library::SRC_IMPORT, 1, &m);
    snprintf(m.imported, sizeof m.imported, "backup-012");
    CHECK(library::format_meta(m, txt, sizeof txt));
    CHECK(!strstr(txt, "unit id"));
    library::parse_meta(txt, &back);
    CHECK_STR(back.imported, "backup-012");
    CHECK(back.source == library::SRC_IMPORT && !back.notes.has_id);
    // too small a buffer: false
    CHECK(!library::format_meta(m, txt, 40));
    // an old backup's .txt (engine::backup): model from its "unit:" line
    library::parse_meta("unit: SMK-37 (calibration record)\nchip key: 980F\nflash id: 856014\n"
                        "unit id: 4150353336323715\nloader: M-Flasher 0.4, CPU 192 MHz\nsize: 1048576\n"
                        "crc32: 0a1b2c3d\nretries: 0\n",
                        &back);
    CHECK_STR(back.model, "SMK-37");
    CHECK_STR(back.name, "");
    CHECK(back.notes.crc == 0x0A1B2C3D && back.source == library::SRC_UNKNOWN);
    library::parse_meta("unit: FM-1 (by flash ID)\r\nchip key: 980F\r\n", &back);
    CHECK_STR(back.model, "FM-1");
    library::parse_meta("unit: unknown\n", &back);
    CHECK_STR(back.model, "");
}

static void test_unit() {
    puts("library: the unit's folder, unit.txt, current.txt");
    char id[32];
    library::unit_id(unit_info(UID_A), 0x12345678, id, sizeof id);
    CHECK_STR(id, "4150353336323715");
    library::unit_id(unit_info(nullptr), 0x12345678, id, sizeof id);
    CHECK_STR(id, "k980F-f12345678");

    static uint8_t trim[0x1000], last[0x1000];
    memset(trim, 0x5A, sizeof trim);
    memset(last, 0xFF, sizeof last);
    for (int i = 0x23C; i < 0x250; i++) last[i] = (uint8_t)i;
    uint32_t h = library::factory_hash(trim, last);
    CHECK(h == library::factory_hash(trim, last));
    CHECK(h != library::factory_hash(nullptr, last));
    last[0x100] ^= 1;  // m-bootloader's TRIMCOPY area: not factory data
    CHECK(h == library::factory_hash(trim, last));
    last[0x240] ^= 1;  // key_mac
    CHECK(h != library::factory_hash(trim, last));
    last[0x240] ^= 1;
    trim[0x10] ^= 1;
    CHECK(h != library::factory_hash(trim, last));

    library::UnitFile u = {}, back;
    snprintf(u.model, sizeof u.model, "SMK-37");
    u.chip_key = 0x980F;
    u.flash_id = 0x856014;
    snprintf(u.label, sizeof u.label, "My Elite");
    char txt[256];
    CHECK(library::format_unit(u, txt, sizeof txt));
    library::parse_unit(txt, &back);
    CHECK_STR(back.model, "SMK-37");
    CHECK_STR(back.label, "My Elite");
    CHECK_STR(back.first_seen, "");
    CHECK(back.chip_key == 0x980F && back.flash_id == 0x856014);

    CHECK(library::format_current(7, "Dub 1", txt, sizeof txt));
    CHECK(library::parse_current(txt) == 7);
    CHECK(library::parse_current("") == 0);
    CHECK(library::parse_current("number: x\n") == 0);
    CHECK(library::parse_current("number: 0\n") == 0);
    CHECK(library::parse_current("name: a\r\nnumber: 12\r\n") == 12);
}

static void test_list() {
    puts("library: the list, newest first; legacy backups; dedupe by CRC");
    jl::Info info = unit_info(UID_A);
    library::Meta m;
    char txt[512];
    std::vector<library::Entry> e(6);
    // snapshots found in directory order, not by number
    const int nums[] = {2, 10, 1};
    const char *names[] = {"Dub 1", "Hip hop 2", "Blank start"};
    const uint32_t crcs[] = {0x22, 0x1010, 0x11};
    for (int i = 0; i < 3; i++) {
        library::meta_for(info, names[i], "", "SMK-37", "", library::SRC_BACKUP, crcs[i], &m);
        CHECK(library::format_meta(m, txt, sizeof txt));
        char base[64];
        library::base_name(nums[i], names[i], base, sizeof base);
        library::snapshot_entry(nums[i], base, txt, &e[i]);
    }
    // old backups: this unit's, another's, one without its .txt
    library::legacy_entry(3, "unit: SMK-37 (x)\nchip key: 980F\nflash id: 856014\nunit id: 4150353336323715\ncrc32: 33\n",
                          info, &e[3]);
    library::legacy_entry(7, "chip key: 980F\nflash id: 856014\nunit id: 4150353336323716\ncrc32: 77\n", info, &e[4]);
    library::legacy_entry(5, nullptr, info, &e[5]);
    CHECK(e[3].whose == library::WHOSE_THIS && e[4].whose == library::WHOSE_OTHER && e[5].whose == library::WHOSE_NO_TXT);
    CHECK_STR(e[3].name, "backup-003");
    CHECK_STR(e[3].model, "SMK-37");

    library::sort(e.data(), (int)e.size(), 2);
    int order[] = {10, 2, 1, 7, 5, 3};
    for (int i = 0; i < 6; i++) CHECK(e[i].number == order[i]);
    CHECK(e[0].kind == library::SNAPSHOT && e[3].kind == library::LEGACY);
    CHECK(e[1].current && !e[0].current && !e[2].current);
    for (int i = 3; i < 6; i++) CHECK(!e[i].current);  // a legacy backup is never current
    CHECK_STR(e[0].name, "Hip hop 2");
    CHECK_STR(e[0].base, "0010 Hip hop 2");

    CHECK(library::next_number(e.data(), 6) == 11);  // legacy numbers do not count
    CHECK(library::next_number(e.data(), 0) == 1);
    CHECK(library::count_snapshots(e.data(), 6) == 3);
    CHECK(library::find_crc(e.data(), 6, 0x22) == 1);
    CHECK(library::find_crc(e.data(), 6, 0x33) == -1);  // only snapshots dedupe
    CHECK(library::find_crc(e.data(), 6, 0x99) == -1);

    // a snapshot without its .txt: listed by its file name, no CRC, never a duplicate
    library::Entry bare;
    library::snapshot_entry(4, "0004 lost", nullptr, &bare);
    CHECK_STR(bare.name, "lost");
    CHECK(!bare.has_crc && bare.whose == library::WHOSE_NO_TXT);
    CHECK(library::find_crc(&bare, 1, 0) == -1);
    // the highest number used up
    library::snapshot_entry(library::NUMBER_MAX, "9999 last", nullptr, &bare);
    CHECK(library::next_number(&bare, 1) == 0);
    // no current
    library::sort(e.data(), 6, 0);
    for (int i = 0; i < 6; i++) CHECK(!e[i].current);

    // whose, without IDs
    restore::Notes n = {};
    n.has_key = n.has_flash = true;
    n.chip_key = 0x980F;
    n.flash_id = 0x856014;
    CHECK(library::whose(&n, info) == library::WHOSE_NO_ID);
    n.flash_id = 0xEB6014;
    CHECK(library::whose(&n, info) == library::WHOSE_OTHER);
    CHECK(library::whose(nullptr, info) == library::WHOSE_NO_TXT);

    CHECK(library::same_model("SMK-37", "SMK-37"));
    CHECK(!library::same_model("SMK-37", "FM-1"));
    CHECK(library::same_model("SMK-37", ""));
}

// A snapshot entry with a .txt, in a folder.
static library::Entry snap_entry(int number, const char *name, uint32_t crc, const char *folder,
                                 const char *date = "") {
    library::Meta m;
    char txt[512], base[64];
    library::meta_for(unit_info(UID_A), name, date, "SMK-37", "", library::SRC_BACKUP, crc, &m);
    CHECK(library::format_meta(m, txt, sizeof txt));
    library::base_name(number, name, base, sizeof base);
    library::Entry e;
    library::snapshot_entry(number, base, txt, &e, folder);
    return e;
}

static void test_folders() {
    puts("library: folders with counts and the newest, a folder's snapshots oldest first");
    std::vector<library::Entry> e;
    e.push_back(snap_entry(5, "SeqAdv 0.1 Dub 1", 0xCA16BF6B, "SeqAdv", "15/11/26"));
    e.push_back(snap_entry(1, "First backup", 0x11223344, ""));
    e.push_back(snap_entry(3, "Stock 1.0", 0x8E21C0D4, "Official Open"));
    e.push_back(snap_entry(7, "SeqAdv 0.1 Hiphop 2", 0x454EFEE3, "seqadv"));  // FAT: the same folder
    e.push_back(snap_entry(4, "SeqAdv 0.1 Blank", 0x20AABDB6, "SeqAdv"));
    library::Entry old;
    library::legacy_entry(9, "chip key: 980F\nflash id: 856014\ncrc32: 99\n", unit_info(UID_A), &old);
    e.push_back(old);
    CHECK_STR(e[0].folder, "SeqAdv");
    CHECK_STR(e[0].date, "15/11/26");
    CHECK_STR(e[1].folder, "");
    CHECK_STR(old.folder, "");

    const char dirs[][library::FOLDER_CHARS + 1] = {"SeqAdv", "Official Open", "Baud Girl", "", "SEQADV"};
    library::Folder f[8];
    int nf = library::folders(e.data(), (int)e.size(), dirs, 5, f, 8);
    CHECK(nf == 4);  // (no folder), then three, alphabetical; "" and a second SeqAdv dropped
    CHECK_STR(f[0].name, "");
    CHECK_STR(f[1].name, "Baud Girl");
    CHECK_STR(f[2].name, "Official Open");
    CHECK_STR(f[3].name, "SeqAdv");
    CHECK(f[0].count == 1 && f[0].newest == 1);  // the old backup is not in "(no folder)"
    CHECK(f[1].count == 0 && f[1].newest == -1);  // an empty folder is listed
    CHECK(f[2].count == 1 && f[2].newest == 2);
    CHECK(f[3].count == 3 && f[3].newest == 3);  // number 7 is the newest
    // no folders on the card: only "(no folder)"
    CHECK(library::folders(e.data(), (int)e.size(), dirs, 0, f, 8) == 1 && f[0].count == 1);
    // room for two: "(no folder)" and the first in the card's order
    CHECK(library::folders(e.data(), (int)e.size(), dirs, 5, f, 2) == 2);
    CHECK(library::folders(e.data(), (int)e.size(), dirs, 5, f, 0) == 0);

    int idx[8];
    int k = library::in_folder(e.data(), (int)e.size(), "SeqAdv", idx, 8);
    CHECK(k == 3 && e[idx[0]].number == 4 && e[idx[1]].number == 5 && e[idx[2]].number == 7);
    CHECK(library::in_folder(e.data(), (int)e.size(), "", idx, 8) == 1 && idx[0] == 1);
    CHECK(library::in_folder(e.data(), (int)e.size(), "Baud Girl", idx, 8) == 0);
    CHECK(library::in_folder(e.data(), (int)e.size(), "SeqAdv", idx, 2) == 2);

    // numbers stay unique across the unit's folders
    CHECK(library::next_number(e.data(), (int)e.size()) == 8);

    char out[80];
    library::label(e[0], out, sizeof out);
    CHECK_STR(out, "SeqAdv / SeqAdv 0.1 Dub 1");
    library::label(e[1], out, sizeof out);
    CHECK_STR(out, "First backup");
    library::label(old, out, sizeof out);
    CHECK_STR(out, "backup-009");
}

static void test_saved_as() {
    puts("library: is what the device holds saved? (LOAD asks \"save first?\" only if not)");
    std::vector<library::Entry> e;
    e.push_back(snap_entry(1, "First backup", 0x11223344, ""));
    e.push_back(snap_entry(2, "Dub 1", 0xCA16BF6B, "SeqAdv"));
    e.push_back(snap_entry(3, "Dub 1 copy", 0xCA16BF6B, "Other"));
    library::Entry bare;
    library::snapshot_entry(4, "0004 lost", nullptr, &bare, "SeqAdv");  // no .txt: no CRC
    e.push_back(bare);
    library::Entry old;
    library::legacy_entry(9, "chip key: 980F\nflash id: 856014\ncrc32: 77777777\n", unit_info(UID_A), &old);
    e.push_back(old);
    int n = (int)e.size();
    // in another folder than "(no folder)": found all the same
    CHECK(library::saved_as(e.data(), n, 0xCA16BF6B, nullptr, 0) == 1);
    CHECK(library::saved_as(e.data(), n, 0x11223344, nullptr, 0) == 0);
    // not there: ask
    CHECK(library::saved_as(e.data(), n, 0xDEADBEEF, nullptr, 0) == -1);
    // an old backup-NNN is not the library: ask
    CHECK(library::saved_as(e.data(), n, 0x77777777, nullptr, 0) == -1);
    // the first one's file failed its check: the copy in another folder counts
    int skip[2] = {1, 2};
    CHECK(library::saved_as(e.data(), n, 0xCA16BF6B, skip, 1) == 2);
    // both failed: not saved, ask
    CHECK(library::saved_as(e.data(), n, 0xCA16BF6B, skip, 2) == -1);
    // an empty library: ask
    CHECK(library::saved_as(e.data(), 0, 0xCA16BF6B, nullptr, 0) == -1);
    // a snapshot without its .txt never counts (its CRC is not known), even CRC 0
    CHECK(library::saved_as(&bare, 1, 0, nullptr, 0) == -1);
}

static void test_battery() {
    puts("library: the battery before a write");
    int p;
    CHECK(library::battery(4100, &p) == library::BATTERY_OK && p == 100);
    CHECK(library::battery(3700, &p) == library::BATTERY_OK && p == 50);
    CHECK(library::battery(3460, &p) == library::BATTERY_OK && p == 20);
    CHECK(library::battery(3450, &p) == library::BATTERY_LOW && p == 18);
    CHECK(library::battery(3000, &p) == library::BATTERY_LOW && p == 0);
    CHECK(library::battery(0, &p) == library::BATTERY_UNKNOWN && p == -1);
    CHECK(library::battery(-1, &p) == library::BATTERY_UNKNOWN);
    CHECK(library::battery(5000, &p) == library::BATTERY_UNKNOWN);
}

// ---------------------------------------------------------------------------
// The switch (LOAD) on the fake unit

struct Snap {
    std::vector<uint8_t> data;
};

static bool snap_read(void *ctx, uint32_t s, uint8_t *buf, char *err, size_t errlen) {
    Snap *f = (Snap *)ctx;
    if ((uint64_t)(s + 1) * SECTOR > f->data.size()) {
        snprintf(err, errlen, "past the end");
        return false;
    }
    memcpy(buf, f->data.data() + s * SECTOR, SECTOR);
    return true;
}

// An SMK-37 (calibration record) with a firmware made from `fw`.
static void make_unit(uint8_t *f, int boot, int fw, int seed = 3) {
    memset(f, 0xFF, SIZE);
    for (uint32_t a = 0; a < 0x4000; a++) f[a] = (uint8_t)(a * 7 + boot);
    for (uint32_t a = 0x4000; a < 0x60000; a++) f[a] = (uint8_t)(a * 13 >> 3) ^ (uint8_t)fw;
    for (uint32_t a = 0xB0000; a < 0xB1000; a++) f[a] = (uint8_t)(a + fw);  // its data (patterns)
    for (uint32_t a = 0xC0000; a < 0xC0100; a++) f[a] = (uint8_t)(a ^ seed);
    for (uint32_t a = 0xC1000; a < 0xC1224; a++) f[a] = (uint8_t)(a * seed + 3);
    memcpy(f + 0xC1224, "trimdat\0", 8);
    for (uint32_t a = 0xFFF40; a < 0xFFF46; a++) f[a] = (uint8_t)(a + seed);
}

struct Run {
    // what the hooks did, and in what order
    std::vector<std::string> steps;
    size_t ops_at_confirm = 99;
    int choice0 = switcher::WRITE, choice1 = switcher::WRITE;
    bool boot_differs_seen = false;
    int battery_calls = 0;
    bool battery_ok = true;
};

static switcher::Choice h_confirm(void *ctx, int stage, const switcher::Report &rep) {
    Run *r = (Run *)ctx;
    r->steps.push_back(stage ? "confirm-exact" : "confirm");
    if (!stage) r->ops_at_confirm = fake::ops.size();
    if (!stage) r->boot_differs_seen = rep.boot_differs;
    return (switcher::Choice)(stage ? r->choice1 : r->choice0);
}
static bool h_battery(void *ctx, char *why, size_t n) {
    Run *r = (Run *)ctx;
    r->battery_calls++;
    r->steps.push_back("battery");
    if (!r->battery_ok) snprintf(why, n, "battery 10 %%");
    return r->battery_ok;
}
static void quiet(const char *, ...) {}
static void verbose(const char *fmt, ...) {
    if (!getenv("VERBOSE")) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
static void no_progress(uint32_t, uint32_t) {}

static switcher::Outcome do_load(Snap &snap, const jl::Info &info, Run &run, const char *snap_model = "SMK-37",
                                 const restore::Notes *notes_override = nullptr, switcher::Report *rep_out = nullptr) {
    static writer::Source src;
    src = {snap_read, &snap, {}};
    restore::Notes notes = {};
    notes.has_key = notes.has_flash = notes.has_crc = notes.has_size = true;
    notes.chip_key = 0x980F;
    notes.flash_id = info.flash_id;
    notes.size = SIZE;
    notes.crc = jl::crc32_update(0, snap.data.data(), snap.data.size());
    memcpy(notes.unique_id, UID_A, 8);
    notes.has_id = true;
    switcher::Request req = {};
    req.info = &info;
    req.smk37 = true;
    req.unit_model = "SMK-37";
    req.snap_model = snap_model;
    req.size = (uint32_t)snap.data.size();
    req.notes = notes_override ? notes_override : &notes;
    req.src = &src;
    req.name = "0002 Dub 1";
    switcher::Hooks h = {};
    h.ctx = &run;
    h.confirm = h_confirm;
    h.battery = h_battery;
    h.log = getenv("VERBOSE") ? verbose : quiet;
    h.progress = no_progress;
    switcher::Report rep;
    char err[128] = "";
    fake::ops.clear();
    switcher::Outcome o = switcher::load(req, h, &rep, err, sizeof err);
    if (getenv("VERBOSE")) printf("    outcome %d: %s\n", o, err);
    if (rep_out) *rep_out = rep;
    return o;
}

static bool protected_untouched() { return !fake::touched(0xC0000, 0xC2000) && !fake::touched(0xFF000, SIZE); }

static void test_load_keeps_boot() {
    puts("load: no auto-save; the snapshot from 0x4000, the boot area kept");
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    std::vector<uint8_t> before(fake::flash, fake::flash + SIZE);
    Snap snap;
    snap.data.resize(SIZE);
    make_unit(snap.data.data(), 9, 0x5A);  // another firmware, another boot area
    jl::Info info = unit_info(UID_A, 0xEB6014);
    Run run;
    switcher::Report rep;
    CHECK(do_load(snap, info, run, "SMK-37", nullptr, &rep) == switcher::WRITTEN);
    CHECK(run.ops_at_confirm == 0);  // nothing erased or programmed before the confirmation
    CHECK(run.steps.size() == 3 && run.steps[0] == "battery" && run.steps[1] == "confirm" &&
          run.steps[2] == "battery");  // no auto-save step any more
    CHECK(run.boot_differs_seen && rep.boot_differs);
    CHECK(rep.start == writer::BOOT_AREA);
    CHECK(rep.check.verdict == restore::SAME_BY_ID);
    CHECK(run.battery_calls == 2);  // at the start and right before writing
    CHECK(!fake::touched(0, 0x4000));
    CHECK(!memcmp(fake::flash, before.data(), 0x4000));  // the unit's own boot area
    CHECK(!memcmp(fake::flash + 0x4000, snap.data.data() + 0x4000, 0xC0000 - 0x4000));
    CHECK(!memcmp(fake::flash + 0xC2000, snap.data.data() + 0xC2000, 0xFF000 - 0xC2000));
    CHECK(protected_untouched());
}

static void test_load_exact() {
    puts("load: \"restore exactly\" writes the boot area too, only when asked");
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    Snap snap;
    snap.data.resize(SIZE);
    make_unit(snap.data.data(), 9, 0x5A);
    jl::Info info = unit_info(UID_A, 0xEB6014);
    Run run;
    run.choice0 = switcher::EXACT;
    switcher::Report rep;
    CHECK(do_load(snap, info, run, "SMK-37", nullptr, &rep) == switcher::WRITTEN);
    CHECK(rep.start == 0);
    CHECK((std::find(run.steps.begin(), run.steps.end(), "confirm-exact") != run.steps.end()));
    CHECK(!memcmp(fake::flash, snap.data.data(), 0xC0000));
    CHECK(protected_untouched());
    // the boot area written last
    std::vector<uint32_t> e = fake::erased_sectors();
    CHECK(!e.empty() && e.back() < 4);

    // exact asked, then not confirmed: nothing written
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    std::vector<uint8_t> before(fake::flash, fake::flash + SIZE);
    Run no;
    no.choice0 = switcher::EXACT;
    no.choice1 = switcher::CANCEL;
    CHECK(do_load(snap, info, no) == switcher::CANCELLED);
    CHECK(fake::ops.empty() && !memcmp(fake::flash, before.data(), SIZE));

    // the boot areas equal: exact is not offered (taken as the default)
    fake::reset();
    make_unit(fake::flash, 9, 0x00);
    Run same;
    same.choice0 = switcher::EXACT;
    CHECK(do_load(snap, info, same, "SMK-37", nullptr, &rep) == switcher::WRITTEN);
    CHECK(!rep.boot_differs && rep.start == writer::BOOT_AREA);
    CHECK((std::find(same.steps.begin(), same.steps.end(), "confirm-exact") == same.steps.end()));
    CHECK(!fake::touched(0, 0x4000));
    CHECK(!memcmp(fake::flash, snap.data.data(), 0xC0000));
}

static void test_load_refused() {
    puts("load: another unit, another model, a bad file: refused, nothing written");
    Snap snap;
    snap.data.resize(SIZE);
    make_unit(snap.data.data(), 9, 0x5A);
    // another unit (another unique ID)
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    std::vector<uint8_t> before(fake::flash, fake::flash + SIZE);
    Run run;
    jl::Info other = unit_info(UID_B, 0xEB6014);
    switcher::Report rep;
    CHECK(do_load(snap, other, run, "SMK-37", nullptr, &rep) == switcher::REFUSED);
    CHECK(rep.check.verdict == restore::OTHER_UNIT);
    CHECK(fake::ops.empty() && !memcmp(fake::flash, before.data(), SIZE));
    CHECK(run.steps.size() == 1 && run.steps[0] == "battery");  // refused before any question
    // another unit's factory data (same ID claimed)
    fake::reset();
    make_unit(fake::flash, 1, 0x00, 7);
    Run r2;
    CHECK(do_load(snap, unit_info(UID_A, 0xEB6014), r2) == switcher::REFUSED);
    CHECK(fake::ops.empty());
    // another model in the snapshot's .txt
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    Run r3;
    CHECK(do_load(snap, unit_info(UID_A, 0xEB6014), r3, "FM-1") == switcher::REFUSED);
    CHECK(fake::ops.empty());
    // damaged: its CRC is not its .txt's
    Snap bad = snap;
    bad.data[0x23456] ^= 1;
    Run r4;
    restore::Notes n = {};
    n.has_key = n.has_flash = n.has_crc = n.has_size = n.has_id = true;
    n.chip_key = 0x980F;
    n.flash_id = 0xEB6014;
    n.size = SIZE;
    n.crc = jl::crc32_update(0, snap.data.data(), SIZE);
    memcpy(n.unique_id, UID_A, 8);
    CHECK(do_load(bad, unit_info(UID_A, 0xEB6014), r4, "SMK-37", &n, &rep) == switcher::REFUSED);
    CHECK(rep.check.verdict == restore::BAD_FILE && fake::ops.empty());
    // truncated
    Snap shortf = snap;
    shortf.data.resize(SIZE / 2);
    Run r5;
    CHECK(do_load(shortf, unit_info(UID_A, 0xEB6014), r5, "SMK-37", &n) == switcher::REFUSED);
    CHECK(fake::ops.empty());
}

static void test_load_wrong_unit_never_saves_or_writes() {
    puts("load: without the auto-save, a wrong unit or file is still refused before anything");
    Snap snap;
    snap.data.resize(SIZE);
    make_unit(snap.data.data(), 9, 0x5A);
    // the snapshot of unit A on unit B, B's flash its own: refused, nothing read into a file, nothing written
    fake::reset();
    make_unit(fake::flash, 1, 0x00, 5);
    std::vector<uint8_t> before(fake::flash, fake::flash + SIZE);
    Run run;
    switcher::Report rep;
    CHECK(do_load(snap, unit_info(UID_B, 0xEB6014), run, "SMK-37", nullptr, &rep) == switcher::REFUSED);
    CHECK(rep.check.verdict == restore::OTHER_UNIT);
    CHECK(fake::ops.empty() && !memcmp(fake::flash, before.data(), SIZE));
    CHECK((std::find(run.steps.begin(), run.steps.end(), "confirm") == run.steps.end()));
    // an FM-1 snapshot offered to the SMK-37: refused by model before the checks read anything
    Run r2;
    CHECK(do_load(snap, unit_info(UID_A, 0xEB6014), r2, "FM-1", nullptr, &rep) == switcher::REFUSED);
    CHECK(fake::ops.empty() && strstr(rep.check.why, "FM-1") != nullptr);
    // no .txt: refused
    Run r3;
    restore::Notes none = {};
    CHECK(do_load(snap, unit_info(UID_A, 0xEB6014), r3, "SMK-37", &none) == switcher::REFUSED);
    CHECK(fake::ops.empty());
}

static void test_load_battery_cancel_nothing() {
    puts("load: low battery, cancel, nothing to write");
    Snap snap;
    snap.data.resize(SIZE);
    make_unit(snap.data.data(), 9, 0x5A);
    jl::Info info = unit_info(UID_A, 0xEB6014);
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    std::vector<uint8_t> before(fake::flash, fake::flash + SIZE);
    Run low;
    low.battery_ok = false;
    CHECK(do_load(snap, info, low) == switcher::BATTERY_LOW);
    CHECK(fake::ops.empty() && low.steps.size() == 1);  // nothing after the battery
    Run cancel;
    cancel.choice0 = switcher::CANCEL;
    CHECK(do_load(snap, info, cancel) == switcher::CANCELLED);
    CHECK(fake::ops.empty() && !memcmp(fake::flash, before.data(), SIZE));
    // the unit already holds it (all but the protected ranges): nothing written, no question
    fake::reset();
    memcpy(fake::flash, snap.data.data(), SIZE);
    memset(fake::flash + 0xC0000, 0x42, 0x100);  // other pairings: never written, not compared
    Run same;
    CHECK(do_load(snap, info, same) == switcher::NOTHING_TO_DO);
    CHECK(fake::ops.empty());
    CHECK((std::find(same.steps.begin(), same.steps.end(), "confirm") == same.steps.end()));
    // the unit cannot be read: stopped, nothing written
    fake::reset();
    make_unit(fake::flash, 1, 0x00);
    Run gone;
    fake::fail_reads = 1;
    CHECK(do_load(snap, info, gone) == switcher::READ_FAILED);
    CHECK(fake::ops.empty());
}

int main() {
    test_names();
    test_suggestion();
    test_folder_names();
    test_meta();
    test_unit();
    test_list();
    test_folders();
    test_saved_as();
    test_battery();
    test_load_keeps_boot();
    test_load_exact();
    test_load_refused();
    test_load_wrong_unit_never_saves_or_writes();
    test_load_battery_cancel_nothing();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
