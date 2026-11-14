// Host tests for the writer (rescue and restore) and the restore's checks, on
// a fake unit (fake_jl.cpp). Plain C++, no framework: tests/host/run.sh.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "fake_jl.h"
#include "restore.h"
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

static constexpr uint32_t SIZE = 0x100000, SECTOR = 0x1000;

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

// ---------------------------------------------------------------------------
// A backup file: bytes in memory, a size, and faults

struct File {
    std::vector<uint8_t> data;
    int fail_at = -1;          // read of this sector fails ...
    int fail_after_reads = 0;  // ... once this many reads have been done
    int reads = 0;
    int flip_at = -1;          // this sector reads differently every other time
};

static bool file_read(void *ctx, uint32_t s, uint8_t *buf, char *err, size_t errlen) {
    File *f = (File *)ctx;
    f->reads++;
    if ((int)s == f->fail_at && f->reads > f->fail_after_reads) {
        snprintf(err, errlen, "SD card: cannot read 0x%05lX", (unsigned long)(s * SECTOR));
        return false;
    }
    if ((uint64_t)(s + 1) * SECTOR > f->data.size()) {
        snprintf(err, errlen, "SD card: past the end at 0x%05lX", (unsigned long)(s * SECTOR));
        return false;
    }
    memcpy(buf, f->data.data() + s * SECTOR, SECTOR);
    if ((int)s == f->flip_at && f->reads % 2) buf[0] ^= 1;
    return true;
}

// ---------------------------------------------------------------------------
// A unit

static const uint8_t UID_A[8] = {0x11, 0x20, 0x05, 0x02, 0x04, 0x01, 0x05, 0x21};
static const uint8_t UID_B[8] = {0x11, 0x20, 0x05, 0x02, 0x04, 0x01, 0x05, 0x22};

// An SMK-37 as it might be: stock-like boot, firmware, settings, BTIF,
// calibration (USRTRIM with trimdat), key_mac. seed varies the unit.
static void make_smk37(uint8_t *f, int seed) {
    memset(f, 0xFF, SIZE);
    for (uint32_t a = 0; a < 0x4000; a++) f[a] = (uint8_t)(a * 7 + 1);           // boot area
    for (uint32_t a = 0x4000; a < 0x90000; a++) f[a] = (uint8_t)(a * 13 >> 3);   // firmware
    for (uint32_t a = 0xB0000; a < 0xB2000; a++) f[a] = (uint8_t)(a + 5);        // settings
    for (uint32_t a = 0xC0000; a < 0xC0100; a++) f[a] = (uint8_t)(a ^ seed);     // BTIF
    for (uint32_t a = 0xC1000; a < 0xC1224; a++) f[a] = (uint8_t)(a * seed + 3); // USRTRIM
    memcpy(f + 0xC1224, "trimdat\0", 8);
    for (uint32_t a = 0xFFF40; a < 0xFFF46; a++) f[a] = (uint8_t)(a + seed);     // key_mac
}

static jl::Info unit_info(const uint8_t *uid) {
    jl::Info i;
    memset(&i, 0, sizeof i);
    i.chip_key = 0x980F;
    i.flash_id = 0xEB6014;
    if (uid) {
        memcpy(i.unique_id, uid, 8);
        i.has_unique_id = true;
    }
    return i;
}

static restore::Notes notes_for(const File &f, const uint8_t *uid) {
    restore::Notes n;
    memset(&n, 0, sizeof n);
    n.has_key = n.has_flash = n.has_crc = n.has_size = true;
    n.chip_key = 0x980F;
    n.flash_id = 0xEB6014;
    n.size = SIZE;
    n.crc = jl::crc32_update(0, f.data.data(), f.data.size());
    if (uid) {
        memcpy(n.unique_id, uid, 8);
        n.has_id = true;
    }
    return n;
}

static restore::Verdict run_check(File &f, const jl::Info &info, const restore::Notes *n, bool smk37 = true,
                                  restore::Result *out = nullptr) {
    writer::Source src = {file_read, &f, {}};
    restore::Result r;
    char err[96];
    bool ok = restore::check(info, smk37, (uint32_t)f.data.size(), n, src, &r, err, sizeof err);
    CHECK(ok);
    if (getenv("VERBOSE")) printf("    verdict %d: %s\n", r.verdict, r.why);
    if (out) *out = r;
    return r.verdict;
}

// plan + apply of file f on the fake unit; false: stopped (err).
static bool restore_file(File &f, writer::Plan *plan, char *err, size_t errlen, bool do_apply = true) {
    static writer::Source src;
    src = {file_read, &f, {}};
    writer::Image img = writer::full_image(writer::SMK37, "backup", &src);
    if (!writer::plan(img, plan, no_progress, err, errlen)) return false;
    if (!do_apply) return true;
    fake::ops.clear();
    return writer::apply(img, *plan, verbose, no_progress, err, errlen);
}

static bool protected_untouched() {
    return !fake::touched(0xC0000, 0xC2000) && !fake::touched(0xFF000, SIZE);
}

// ---------------------------------------------------------------------------
// Tests

static void test_notes() {
    puts("notes: the .txt engine::backup writes");
    const char *txt =
        "unit: SMK-37 (calibration record)\nchip key: 980F\nflash id: EB6014\nunit id: 1120050204010521\n"
        "loader: M-Flasher 0.4, CPU 192 MHz\nsize: 1048576\ncrc32: 0a1b2c3d\nretries: 0\n";
    restore::Notes n;
    restore::parse_notes(txt, &n);
    CHECK(n.has_key && n.chip_key == 0x980F);
    CHECK(n.has_flash && n.flash_id == 0xEB6014);
    CHECK(n.has_id && !memcmp(n.unique_id, UID_A, 8));
    CHECK(n.has_size && n.size == 1048576);
    CHECK(n.has_crc && n.crc == 0x0A1B2C3D);
    restore::parse_notes("unit: FM-1 (by flash ID)\r\nchip key: 980F\r\nflash id: 856014\r\ncrc32: 1\r\n", &n);
    CHECK(n.has_key && n.has_flash && n.flash_id == 0x856014 && !n.has_id && n.has_crc && n.crc == 1);
    restore::parse_notes("unit id: 11200502\nchip key: zz\n", &n);
    CHECK(!n.has_id && !n.has_key);
}

static void test_rescue_unchanged() {
    puts("rescue: the existing repair still does what it did");
    fake::reset();
    make_smk37(fake::flash, 3);
    std::vector<uint8_t> before(fake::flash, fake::flash + SIZE);
    const writer::Image &img = writer::image(writer::SMK37);
    char err[128];
    CHECK(writer::image_ok(img, err, sizeof err));
    writer::Plan plan;
    CHECK(writer::plan(img, &plan, no_progress, err, sizeof err));
    CHECK(plan.to_write == 4);  // the boot area
    CHECK(plan.to_erase > 0);
    CHECK(writer::apply(img, plan, quiet, no_progress, err, sizeof err));
    CHECK(protected_untouched());
    CHECK(!memcmp(fake::flash, img.boot, 0x4000));
    bool rest_blank = true;
    for (uint32_t a = 0x4000; a < 0xFF000; a++)
        if ((a < 0xC0000 || a >= 0xC2000) && fake::flash[a] != 0xFF) rest_blank = false;
    CHECK(rest_blank);
    CHECK(!memcmp(fake::flash + 0xC0000, before.data() + 0xC0000, 0x2000));
    CHECK(!memcmp(fake::flash + 0xFF000, before.data() + 0xFF000, 0x1000));
    // ascending order, as before (the boot area first)
    std::vector<uint32_t> e = fake::erased_sectors();
    CHECK(!e.empty() && e.front() == 0);
    CHECK(writer::plan(img, &plan, no_progress, err, sizeof err));
    CHECK(plan.to_write == 0 && plan.to_erase == 0);
}

static void test_identical() {
    puts("restore: an identical image writes nothing");
    fake::reset();
    make_smk37(fake::flash, 3);
    File f;
    f.data.assign(fake::flash, fake::flash + SIZE);
    writer::Plan plan;
    char err[128];
    CHECK(restore_file(f, &plan, err, sizeof err));
    CHECK(plan.to_write == 0 && plan.to_erase == 0 && plan.runs == 0);
    int writes = 0;
    for (const fake::Op &o : fake::ops) writes += o.kind == 'e' || o.kind == 'p';
    CHECK(writes == 0);
}

static void test_differing_only() {
    puts("restore: only the differing sectors are written, the boot area last");
    fake::reset();
    make_smk37(fake::flash, 3);
    File f;
    f.data.assign(fake::flash, fake::flash + SIZE);
    // the unit as the repair left it: m-bootloader's boot area, firmware erased
    // from 0x10000, plus m-bootloader's TRIMCOPY at 0xFF000
    for (uint32_t a = 0x1000; a < 0x3000; a++) fake::flash[a] ^= 0x5A;
    memset(fake::flash + 0x10000, 0xFF, 0x20000);
    memcpy(fake::flash + 0xFF000, "TRIMCOPY", 8);
    memset(fake::flash + 0xB1000, 0x00, 0x1000);  // a setting changed
    writer::Plan plan;
    char err[128];
    CHECK(restore_file(f, &plan, err, sizeof err));
    CHECK(plan.to_write == 2 + 32 + 1);
    CHECK(plan.to_erase == 0);
    std::vector<uint32_t> e = fake::erased_sectors();
    std::vector<uint32_t> want;
    for (uint32_t s = 0x10; s < 0x30; s++) want.push_back(s);
    want.push_back(0xB1);
    want.push_back(1);
    want.push_back(2);
    CHECK(e == want);
    CHECK(protected_untouched());
    CHECK(!memcmp(fake::flash, f.data.data(), 0xC0000));
    CHECK(!memcmp(fake::flash + 0xC2000, f.data.data() + 0xC2000, 0xFF000 - 0xC2000));
    CHECK(!memcmp(fake::flash + 0xFF000, "TRIMCOPY", 8));  // kept: never written
    CHECK(restore_file(f, &plan, err, sizeof err, false));
    CHECK(plan.to_write == 0 && plan.to_erase == 0);
    // a blank sector in the file is an erase only
    fake::reset();
    make_smk37(fake::flash, 3);
    f.data.assign(fake::flash, fake::flash + SIZE);
    memset(f.data.data() + 0x50000, 0xFF, 0x1000);
    CHECK(restore_file(f, &plan, err, sizeof err));
    CHECK(plan.to_erase == 1 && plan.to_write == 0);
    CHECK(fake::ops.size() == 1 && fake::ops[0].kind == 'e' && fake::ops[0].addr == 0x50000);
}

static void test_protected() {
    puts("restore: protected ranges are never written, even where the file differs");
    fake::reset();
    make_smk37(fake::flash, 3);
    std::vector<uint8_t> unit(fake::flash, fake::flash + SIZE);
    File f;
    f.data = unit;
    for (uint32_t a = 0xC0000; a < 0xC2000; a++) f.data[a] ^= 0xA5;
    for (uint32_t a = 0xFF000; a < SIZE; a++) f.data[a] = 0x00;
    f.data[0x20000] ^= 1;  // and one ordinary sector
    writer::Plan plan;
    char err[128];
    CHECK(restore_file(f, &plan, err, sizeof err));
    CHECK(plan.to_write == 1);
    for (uint32_t s = 0xC0; s < 0xC2; s++) CHECK(!plan.change[s]);
    CHECK(protected_untouched());
    CHECK(!memcmp(fake::flash + 0xC0000, unit.data() + 0xC0000, 0x2000));
    CHECK(!memcmp(fake::flash + 0xFF000, unit.data() + 0xFF000, 0x1000));
    // the FM-1's image writes 0xC0000-0xC1FFF (not protected there), never 0xFF000-
    fake::reset();
    make_smk37(fake::flash, 3);
    static writer::Source src;
    src = {file_read, &f, {}};
    writer::Image img = writer::full_image(writer::FM1, "backup", &src);
    CHECK(writer::plan(img, &plan, no_progress, err, sizeof err));
    CHECK(plan.change[0xC0] && plan.change[0xC1]);
    fake::ops.clear();
    CHECK(writer::apply(img, plan, quiet, no_progress, err, sizeof err));
    CHECK(!fake::touched(0xFF000, SIZE));
}

static void test_same_unit() {
    puts("check: the same unit, another unit, cannot tell");
    fake::reset();
    make_smk37(fake::flash, 3);
    File f;
    f.data.assign(fake::flash, fake::flash + SIZE);
    restore::Notes n = notes_for(f, UID_A);

    CHECK(run_check(f, unit_info(UID_A), &n) == restore::SAME_BY_ID);
    CHECK(run_check(f, unit_info(UID_B), &n) == restore::OTHER_UNIT);

    // after the repair and a new firmware: m-bootloader's TRIMCOPY at
    // 0xFF000, other pairings, the boot area and firmware changed: still this unit
    memcpy(fake::flash + 0xFF000, "TRIMCOPY", 8);
    memcpy(fake::flash + 0xFF010, f.data.data() + 0xC1000, 0x22C);
    memset(fake::flash + 0xC0000, 0x42, 0x80);
    memset(fake::flash, 0x33, 0x9000);
    CHECK(run_check(f, unit_info(UID_A), &n) == restore::SAME_BY_ID);
    restore::Notes old = notes_for(f, nullptr);  // a backup without the ID
    CHECK(run_check(f, unit_info(UID_A), &old) == restore::SAME_BY_FACTORY);
    CHECK(run_check(f, unit_info(nullptr), &n) == restore::SAME_BY_FACTORY);
    CHECK(restore::allowed(restore::SAME_BY_FACTORY) && !restore::allowed(restore::OTHER_UNIT));

    // another SMK-37 (its own calibration and key_mac), no IDs: refused
    make_smk37(fake::flash, 9);
    CHECK(run_check(f, unit_info(nullptr), &old) == restore::OTHER_UNIT);
    // only the calibration differs; only key_mac differs
    make_smk37(fake::flash, 3);
    fake::flash[0xC1100] ^= 1;
    CHECK(run_check(f, unit_info(nullptr), &old) == restore::OTHER_UNIT);
    CHECK(run_check(f, unit_info(UID_A), &n) == restore::OTHER_UNIT);  // same ID, other calibration: refused too
    make_smk37(fake::flash, 3);
    fake::flash[0xFFF41] ^= 1;
    CHECK(run_check(f, unit_info(nullptr), &old) == restore::OTHER_UNIT);

    // nothing unit-unique and no ID: cannot tell
    fake::reset();
    File blank;
    blank.data.assign(SIZE, 0xFF);
    for (uint32_t a = 0; a < 0x4000; a++) blank.data[a] = (uint8_t)a;
    memcpy(fake::flash, blank.data.data(), SIZE);
    restore::Notes bn = notes_for(blank, nullptr);
    CHECK(run_check(blank, unit_info(nullptr), &bn) == restore::CANNOT_TELL);
    CHECK(run_check(blank, unit_info(nullptr), &bn, false) == restore::CANNOT_TELL);

    // another chip or chip key
    make_smk37(fake::flash, 3);
    jl::Info other = unit_info(UID_A);
    other.flash_id = 0x856014;
    CHECK(run_check(f, other, &n) == restore::OTHER_UNIT);
    other = unit_info(UID_A);
    other.chip_key = 0x1234;
    CHECK(run_check(f, other, &n) == restore::OTHER_UNIT);
    // an SMK-37's backup on a unit taken for an FM-1
    CHECK(run_check(f, unit_info(UID_A), &n, false) == restore::OTHER_UNIT);

    // the unit cannot be read: no verdict, an error
    fake::fail_reads = 1;
    writer::Source src = {file_read, &f, {}};
    restore::Result r;
    char err[96];
    CHECK(!restore::check(unit_info(UID_A), true, SIZE, &n, src, &r, err, sizeof err));
}

static void test_bad_files() {
    puts("check: truncated, oversized, unreadable, damaged files");
    fake::reset();
    make_smk37(fake::flash, 3);
    File good;
    good.data.assign(fake::flash, fake::flash + SIZE);
    restore::Notes n = notes_for(good, UID_A);
    jl::Info info = unit_info(UID_A);

    File f = good;
    f.data.resize(SIZE - 1);
    CHECK(run_check(f, info, &n) == restore::BAD_FILE);
    f.data.resize(SIZE / 2);
    CHECK(run_check(f, info, &n) == restore::BAD_FILE);
    f = good;
    f.data.push_back(0);
    CHECK(run_check(f, info, &n) == restore::BAD_FILE);
    f = good;
    f.fail_at = 0x80;
    CHECK(run_check(f, info, &n) == restore::BAD_FILE);
    f = good;
    f.flip_at = 0x30;  // reads differently each pass
    CHECK(run_check(f, info, &n) == restore::BAD_FILE);
    f = good;
    f.data[0x12345] ^= 0x10;  // not what its .txt says
    CHECK(run_check(f, info, &n) == restore::BAD_FILE);
    CHECK(run_check(good, info, nullptr) == restore::BAD_FILE);  // no .txt
    restore::Notes partial = n;
    partial.has_crc = false;
    CHECK(run_check(good, info, &partial) == restore::BAD_FILE);
    partial = n;
    partial.size = SIZE * 2;
    CHECK(run_check(good, info, &partial) == restore::BAD_FILE);
    restore::Result r;
    CHECK(run_check(good, info, &n, true, &r) == restore::SAME_BY_ID && r.crc == n.crc);
}

static void test_read_error_while_writing() {
    puts("restore: an SD read error while writing stops before erasing more");
    fake::reset();
    make_smk37(fake::flash, 3);
    File f;
    f.data.assign(fake::flash, fake::flash + SIZE);
    for (uint32_t a = 0; a < 0x4000; a++) fake::flash[a] ^= 0xFF;  // boot area differs
    memset(fake::flash + 0x20000, 0xFF, 0x10000);                    // 16 sectors erased
    std::vector<uint8_t> unit(fake::flash, fake::flash + SIZE);
    // plan reads each sector once: the failure comes on the next read, in apply
    f.fail_at = 0x26;
    f.fail_after_reads = 1 << 30;
    writer::Plan plan;
    char err[128];
    static writer::Source src;
    src = {file_read, &f, {}};
    writer::Image img = writer::full_image(writer::SMK37, "backup", &src);
    CHECK(writer::plan(img, &plan, no_progress, err, sizeof err));
    CHECK(plan.to_write == 4 + 16);
    f.fail_after_reads = f.reads;
    fake::ops.clear();
    CHECK(!writer::apply(img, plan, quiet, no_progress, err, sizeof err));
    if (getenv("VERBOSE")) printf("    err: %s\n", err);
    CHECK(strstr(err, "0x26000") != nullptr);
    // pieces of 4: 0x20-0x23 written; 0x24-0x27 (holds 0x26) never erased; nothing after
    std::vector<uint32_t> e = fake::erased_sectors();
    CHECK((e == std::vector<uint32_t>{0x20, 0x21, 0x22, 0x23}));
    CHECK(!memcmp(fake::flash + 0x20000, f.data.data() + 0x20000, 0x4000));
    CHECK(!memcmp(fake::flash + 0x24000, unit.data() + 0x24000, SIZE - 0x24000));
    CHECK(!memcmp(fake::flash, unit.data(), 0x4000));  // the boot area: still the unit's
    // the card back: a new plan and apply finish it
    f.fail_at = -1;
    CHECK(writer::plan(img, &plan, no_progress, err, sizeof err));
    CHECK(plan.to_write == 4 + 12);
    CHECK(writer::apply(img, plan, quiet, no_progress, err, sizeof err));
    CHECK(!memcmp(fake::flash, f.data.data(), 0xC0000));
}

static void test_file_changed() {
    puts("restore: a file that changed after the plan is not written");
    fake::reset();
    make_smk37(fake::flash, 3);
    File f;
    f.data.assign(fake::flash, fake::flash + SIZE);
    memset(fake::flash + 0x40000, 0xFF, 0x2000);
    std::vector<uint8_t> unit(fake::flash, fake::flash + SIZE);
    writer::Plan plan;
    char err[128];
    static writer::Source src;
    src = {file_read, &f, {}};
    writer::Image img = writer::full_image(writer::SMK37, "backup", &src);
    CHECK(writer::plan(img, &plan, no_progress, err, sizeof err));
    f.data[0x41010] ^= 1;  // another card, another file of the same name
    fake::ops.clear();
    CHECK(!writer::apply(img, plan, quiet, no_progress, err, sizeof err));
    CHECK(strstr(err, "differently") != nullptr);
    CHECK(fake::ops.empty());
    CHECK(!memcmp(fake::flash, unit.data(), SIZE));
}

static void test_final_check_reads_file() {
    puts("restore: the final check reads the file again");
    fake::reset();
    make_smk37(fake::flash, 3);
    File f;
    f.data.assign(fake::flash, fake::flash + SIZE);
    fake::flash[0x60000] ^= 1;
    writer::Plan plan;
    char err[128];
    CHECK(restore_file(f, &plan, err, sizeof err));
    // reads: plan (every writable sector), the piece, the final check (again all)
    int writable = writer::SECTORS - 2;
    CHECK(f.reads == writable + 1 + writable);
}

int main() {
    test_notes();
    test_rescue_unchanged();
    test_identical();
    test_differing_only();
    test_protected();
    test_same_unit();
    test_bad_files();
    test_read_error_while_writing();
    test_file_changed();
    test_final_check_reads_file();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
