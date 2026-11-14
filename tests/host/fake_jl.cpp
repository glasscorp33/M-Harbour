#include "fake_jl.h"

#include <stdio.h>
#include <string.h>

namespace fake {
uint8_t flash[SIZE];
std::vector<Op> ops;
int fail_reads;

void reset() {
    memset(flash, 0xFF, SIZE);
    ops.clear();
    fail_reads = 0;
}

bool touched(uint32_t lo, uint32_t hi) {
    for (const Op &o : ops)
        if (o.addr < hi && o.addr + o.len > lo) return true;
    return false;
}

std::vector<uint32_t> erased_sectors() {
    std::vector<uint32_t> v;
    for (const Op &o : ops)
        if (o.kind == 'e')
            for (uint32_t a = o.addr; a < o.addr + o.len; a += 0x1000) v.push_back(a / 0x1000);
    return v;
}
}  // namespace fake

namespace jl {

using fake::flash;

// the same CRC as src/jl.cpp (which needs the USB host, so it is not built here)
uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static bool range(uint32_t addr, uint32_t n, char *err, size_t errlen) {
    if (addr + n > fake::SIZE || addr + n < addr) {
        snprintf(err, errlen, "out of range");
        return false;
    }
    return true;
}

bool read(uint32_t addr, uint8_t *buf, uint32_t n, char *err, size_t errlen) {
    if (fake::fail_reads > 0) {
        fake::fail_reads--;
        snprintf(err, errlen, "fake read failure");
        return false;
    }
    if (!range(addr, n, err, errlen)) return false;
    memcpy(buf, flash + addr, n);
    return true;
}

bool crc32(uint32_t addr, uint32_t n, uint32_t *crc, char *err, size_t errlen) {
    if (!range(addr, n, err, errlen)) return false;
    *crc = crc32_update(0, flash + addr, n);
    return true;
}

bool erase(uint32_t addr, uint32_t n, char *err, size_t errlen) {
    if (!range(addr, n, err, errlen) || addr % 0x1000 || n % 0x1000) return false;
    memset(flash + addr, 0xFF, n);
    fake::ops.push_back({'e', addr, n});
    return true;
}

bool program(uint32_t addr, const uint8_t *data, uint32_t n, char *err, size_t errlen) {
    if (!range(addr, n, err, errlen)) return false;
    for (uint32_t i = 0; i < n; i++) flash[addr + i] &= data[i];
    fake::ops.push_back({'p', addr, n});
    return true;
}

}  // namespace jl
