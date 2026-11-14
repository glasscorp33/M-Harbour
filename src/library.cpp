#include "library.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace library {

// ---------------------------------------------------------------------------
// Names

void clean_name(const char *in, char *out, size_t outlen) {
    if (!outlen) return;
    size_t n = 0, cap = outlen - 1 < NAME_CHARS ? outlen - 1 : NAME_CHARS;
    bool space = false;
    for (const char *s = in ? in : ""; *s && n < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F) continue;  // control characters (a newline would break the .txt)
        if (c == ' ') {
            space = n > 0;  // leading spaces dropped, runs made one
            continue;
        }
        if (space) {
            if (n + 1 >= cap) break;
            out[n++] = ' ';
            space = false;
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
}

static bool fat_bad(unsigned char c) { return c < 0x20 || c >= 0x7F || strchr("/\\:*?\"<>|", c); }

void safe_name(const char *name, char *out, size_t outlen) {
    if (!outlen) return;
    char clean[NAME_CHARS + 1];
    clean_name(name, clean, sizeof clean);
    size_t cap = outlen - 1 < SAFE_MAX ? outlen - 1 : SAFE_MAX, n = 0;
    for (const char *s = clean; *s && n < cap; s++) out[n++] = fat_bad((unsigned char)*s) ? '-' : *s;
    // FAT drops trailing dots and spaces from a name: none, so it reads back as written
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = 0;
    if (!n) snprintf(out, outlen, "snapshot");
}

void base_name(int number, const char *name, char *out, size_t outlen) {
    char safe[SAFE_MAX + 1];
    safe_name(name, safe, sizeof safe);
    snprintf(out, outlen, "%04d %s", number, safe);
}

static bool ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)s[n - e + i]) != tolower((unsigned char)ext[i])) return false;
    return true;
}

bool parse_snapshot_file(const char *file, const char *ext, int *number, char *base, size_t baselen) {
    size_t n = strlen(file), e = strlen(ext);
    if (n < 6 + e || !ends_with(file, ext)) return false;
    for (int i = 0; i < 4; i++)
        if (!isdigit((unsigned char)file[i])) return false;
    if (file[4] != ' ') return false;
    int num = atoi(file);
    if (num < 1 || n - e >= baselen) return false;
    memcpy(base, file, n - e);
    base[n - e] = 0;
    *number = num;
    return true;
}

bool parse_legacy_file(const char *file, int *number) {
    bool ok = strlen(file) == 14 && !strncmp(file, "backup-", 7) && isdigit((unsigned char)file[7]) &&
              isdigit((unsigned char)file[8]) && isdigit((unsigned char)file[9]) && !strcmp(file + 10, ".bin");
    if (!ok || atoi(file + 7) < 1) return false;
    *number = atoi(file + 7);
    return true;
}

void suggest(const char *firmware, const char *date, const char *model, char *out, size_t outlen) {
    char fw[NAME_CHARS + 1], d[NAME_CHARS + 1], m[NAME_CHARS + 1], s[3 * (NAME_CHARS + 1)];
    clean_name(firmware, fw, sizeof fw);
    clean_name(date, d, sizeof d);
    clean_name(model, m, sizeof m);
    const char *first = fw[0] ? fw : m[0] ? m : d[0] ? "" : "snapshot";
    snprintf(s, sizeof s, "%s%s%s", first, first[0] && d[0] ? " " : "", d);
    clean_name(s, out, outlen);
}

static bool starts_auto(const char *s) { return !strncmp(s, "auto ", 5) || !strncmp(s, "Auto ", 5); }

void auto_name(const char *current, const char *firmware, const char *date, const char *model, char *out,
               size_t outlen) {
    char cur[NAME_CHARS + 1], d[NAME_CHARS + 1], s[2 * NAME_CHARS + 16];
    clean_name(current, cur, sizeof cur);
    clean_name(date, d, sizeof d);
    const char *c = cur;
    while (starts_auto(c)) c += 5;
    if (*c) snprintf(s, sizeof s, "auto %s%s%s", c, d[0] ? " " : "", d);
    else {
        char sug[NAME_CHARS + 1];
        suggest(firmware, d, model, sug, sizeof sug);
        snprintf(s, sizeof s, "auto %s", sug);
    }
    clean_name(s, out, outlen);
}

// ---------------------------------------------------------------------------
// The .txt files

const char *source_name(Source s) {
    switch (s) {
        case SRC_BACKUP: return "backup";
        case SRC_AUTO: return "auto-save";
        case SRC_IMPORT: return "import";
        default: return "";
    }
}

static Source parse_source(const char *v) {
    if (!strcmp(v, "backup")) return SRC_BACKUP;
    if (!strcmp(v, "auto-save")) return SRC_AUTO;
    if (!strcmp(v, "import")) return SRC_IMPORT;
    return SRC_UNKNOWN;
}

// Calls f(key, value) for each "key: value" line (value trimmed).
template <typename F>
static void each_line(const char *text, F f) {
    for (const char *line = text; line && *line;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *colon = (const char *)memchr(line, ':', len);
        if (colon) {
            char key[24], val[96];
            size_t kl = colon - line, vl;
            const char *v = colon + 1;
            while (v < line + len && *v == ' ') v++;
            vl = line + len - v;
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (kl < sizeof key && vl < sizeof val) {
                memcpy(key, line, kl);
                key[kl] = 0;
                memcpy(val, v, vl);
                val[vl] = 0;
                f(key, val);
            }
        }
        line = eol ? eol + 1 : nullptr;
    }
}

static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src); }

// "SMK-37 (calibration record)" -> "SMK-37"; "unknown" -> ""
static void model_of_unit_line(const char *v, char *out, size_t n) {
    if (!strncmp(v, "SMK-37", 6)) copy(out, n, "SMK-37");
    else if (!strncmp(v, "FM-1", 4)) copy(out, n, "FM-1");
    else out[0] = 0;
}

void parse_meta(const char *text, Meta *m) {
    memset(m, 0, sizeof *m);
    restore::parse_notes(text, &m->notes);
    bool has_model = false;
    each_line(text, [&](const char *k, const char *v) {
        if (!strcmp(k, "name")) clean_name(v, m->name, sizeof m->name);
        else if (!strcmp(k, "date")) copy(m->date, sizeof m->date, v);
        else if (!strcmp(k, "model")) {
            copy(m->model, sizeof m->model, v);
            has_model = true;
        } else if (!strcmp(k, "unit") && !has_model) model_of_unit_line(v, m->model, sizeof m->model);
        else if (!strcmp(k, "firmware")) copy(m->firmware, sizeof m->firmware, v);
        else if (!strcmp(k, "loader")) copy(m->loader, sizeof m->loader, v);
        else if (!strcmp(k, "source")) m->source = parse_source(v);
        else if (!strcmp(k, "imported from")) copy(m->imported, sizeof m->imported, v);
    });
}

bool format_meta(const Meta &m, char *buf, size_t buflen) {
    char name[NAME_CHARS + 1];
    clean_name(m.name, name, sizeof name);
    size_t n = 0;
    auto put = [&](const char *fmt, auto... args) {
        if (n >= buflen) return;
        int w = snprintf(buf + n, buflen - n, fmt, args...);
        n = w < 0 ? buflen : n + (size_t)w;
    };
    put("name: %s\n", name);
    put("date: %s\n", m.date);
    put("model: %s\n", m.model);
    put("firmware: %s\n", m.firmware);
    put("source: %s\n", source_name(m.source));
    if (m.imported[0]) put("imported from: %s\n", m.imported);
    if (m.notes.has_key) put("chip key: %04X\n", m.notes.chip_key);
    if (m.notes.has_flash) put("flash id: %06lX\n", (unsigned long)m.notes.flash_id);
    if (m.notes.has_id) {
        put("%s", "unit id: ");
        for (int i = 0; i < 8; i++) put("%02x", m.notes.unique_id[i]);
        put("%s", "\n");
    }
    if (m.loader[0]) put("loader: %s\n", m.loader);
    if (m.notes.has_size) put("size: %lu\n", (unsigned long)m.notes.size);
    if (m.notes.has_crc) put("crc32: %08lx\n", (unsigned long)m.notes.crc);
    return n < buflen;
}

void meta_for(const jl::Info &info, const char *name, const char *date, const char *model, const char *firmware,
              Source source, uint32_t crc, Meta *m) {
    memset(m, 0, sizeof *m);
    clean_name(name, m->name, sizeof m->name);
    copy(m->date, sizeof m->date, date ? date : "");
    copy(m->model, sizeof m->model, model ? model : "");
    clean_name(firmware, m->firmware, sizeof m->firmware);
    if (info.loader[0]) snprintf(m->loader, sizeof m->loader, "M-Flasher %s, CPU %u MHz", info.loader, info.mhz);
    m->source = source;
    restore::Notes &n = m->notes;
    n.has_key = n.has_flash = n.has_crc = n.has_size = true;
    n.chip_key = info.chip_key;
    n.flash_id = info.flash_id;
    n.crc = crc;
    n.size = restore::FLASH_SIZE;
    if (info.has_unique_id) {
        memcpy(n.unique_id, info.unique_id, 8);
        n.has_id = true;
    }
}

// ---------------------------------------------------------------------------
// The unit

void unit_id(const jl::Info &info, uint32_t factory_hash, char *out, size_t outlen) {
    if (info.has_unique_id) {
        char id[17];
        for (int i = 0; i < 8; i++) snprintf(id + 2 * i, 3, "%02x", info.unique_id[i]);
        snprintf(out, outlen, "%s", id);
    } else {
        snprintf(out, outlen, "k%04X-f%08lx", info.chip_key, (unsigned long)factory_hash);
    }
}

uint32_t factory_hash(const uint8_t *usrtrim, const uint8_t *last_sector) {
    uint32_t crc = 0;
    if (usrtrim) crc = jl::crc32_update(crc, usrtrim, restore::USRTRIM_END - restore::USRTRIM);
    uint32_t off = restore::KEY_MAC % 0x1000;
    return jl::crc32_update(crc, last_sector + off, 0x1000 - off);
}

bool format_unit(const UnitFile &u, char *buf, size_t buflen) {
    char label[NAME_CHARS + 1];
    clean_name(u.label, label, sizeof label);
    int w = snprintf(buf, buflen, "model: %s\nchip key: %04X\nflash id: %06lX\nfirst seen: %s\nlabel: %s\n", u.model,
                     u.chip_key, (unsigned long)u.flash_id, u.first_seen, label);
    return w >= 0 && (size_t)w < buflen;
}

void parse_unit(const char *text, UnitFile *u) {
    memset(u, 0, sizeof *u);
    restore::Notes n;
    restore::parse_notes(text, &n);
    u->chip_key = n.chip_key;
    u->flash_id = n.flash_id;
    each_line(text, [&](const char *k, const char *v) {
        if (!strcmp(k, "model")) copy(u->model, sizeof u->model, v);
        else if (!strcmp(k, "first seen")) copy(u->first_seen, sizeof u->first_seen, v);
        else if (!strcmp(k, "label")) clean_name(v, u->label, sizeof u->label);
    });
}

bool format_current(int number, const char *name, char *buf, size_t buflen) {
    char clean[NAME_CHARS + 1];
    clean_name(name, clean, sizeof clean);
    int w = snprintf(buf, buflen, "number: %d\nname: %s\n", number, clean);
    return w >= 0 && (size_t)w < buflen;
}

int parse_current(const char *text) {
    int number = 0;
    each_line(text, [&](const char *k, const char *v) {
        if (!strcmp(k, "number")) {
            char *end;
            long x = strtol(v, &end, 10);
            if (end != v && !*end && x >= 1 && x <= NUMBER_MAX) number = (int)x;
        }
    });
    return number;
}

// ---------------------------------------------------------------------------
// The list

Whose whose(const restore::Notes *notes, const jl::Info &info) {
    if (!notes) return WHOSE_NO_TXT;
    if ((notes->has_key && notes->chip_key != info.chip_key) || (notes->has_flash && notes->flash_id != info.flash_id))
        return WHOSE_OTHER;
    if (!notes->has_id || !info.has_unique_id) return WHOSE_NO_ID;
    return memcmp(notes->unique_id, info.unique_id, 8) ? WHOSE_OTHER : WHOSE_THIS;
}

const char *whose_name(Whose w) {
    switch (w) {
        case WHOSE_THIS: return "this unit";
        case WHOSE_OTHER: return "OTHER unit";
        case WHOSE_NO_ID: return "no unit ID";
        default: return "no .txt";
    }
}

void snapshot_entry(int number, const char *base, const char *text, Entry *e) {
    memset(e, 0, sizeof *e);
    e->kind = SNAPSHOT;
    e->number = number;
    copy(e->base, sizeof e->base, base);
    e->whose = WHOSE_THIS;
    if (text) {
        static Meta m;  // large: not on the stack
        parse_meta(text, &m);
        copy(e->name, sizeof e->name, m.name);
        e->has_crc = m.notes.has_crc;
        e->crc = m.notes.crc;
        e->source = m.source;
        copy(e->model, sizeof e->model, m.model);
    } else {
        e->whose = WHOSE_NO_TXT;
    }
    if (!e->name[0]) copy(e->name, sizeof e->name, strlen(base) > 5 ? base + 5 : base);
}

void legacy_entry(int number, const char *text, const jl::Info &info, Entry *e) {
    memset(e, 0, sizeof *e);
    e->kind = LEGACY;
    e->number = number;
    snprintf(e->base, sizeof e->base, "backup-%03d", number);
    snprintf(e->name, sizeof e->name, "backup-%03d", number);
    if (!text) {
        e->whose = WHOSE_NO_TXT;
        return;
    }
    static Meta m;
    parse_meta(text, &m);
    e->has_crc = m.notes.has_crc;
    e->crc = m.notes.crc;
    copy(e->model, sizeof e->model, m.model);
    e->whose = whose(&m.notes, info);
}

static bool before(const Entry &a, const Entry &b) {
    if (a.kind != b.kind) return a.kind == SNAPSHOT;
    return a.number > b.number;
}

void sort(Entry *e, int n, int current) {
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && before(e[j], e[j - 1]); j--) {
            Entry t = e[j];
            e[j] = e[j - 1];
            e[j - 1] = t;
        }
    }
    for (int i = 0; i < n; i++) e[i].current = e[i].kind == SNAPSHOT && current && e[i].number == current;
}

int next_number(const Entry *e, int n) {
    int max = 0;
    for (int i = 0; i < n; i++)
        if (e[i].kind == SNAPSHOT && e[i].number > max) max = e[i].number;
    return max < NUMBER_MAX ? max + 1 : 0;
}

int find_crc(const Entry *e, int n, uint32_t crc) {
    for (int i = 0; i < n; i++)
        if (e[i].kind == SNAPSHOT && e[i].has_crc && e[i].crc == crc) return i;
    return -1;
}

int count_snapshots(const Entry *e, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c += e[i].kind == SNAPSHOT;
    return c;
}

bool same_model(const char *unit_model, const char *snap_model) {
    if (!unit_model || !snap_model || !unit_model[0] || !snap_model[0]) return true;
    return !strcmp(unit_model, snap_model);
}

Battery battery(int mv, int *percent) {
    if (mv < 2500 || mv > 4500) {
        *percent = -1;
        return BATTERY_UNKNOWN;
    }
    int p = (mv - 3300) * 100 / 800;
    *percent = p < 0 ? 0 : p > 100 ? 100 : p;
    return *percent < BATTERY_MIN_PERCENT ? BATTERY_LOW : BATTERY_OK;
}

}  // namespace library
