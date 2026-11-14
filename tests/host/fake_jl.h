// A fake unit for the host tests: jl:: on a 1 MiB array, with NOR rules
// (erase sets 0xFF, program can only clear bits) and a record of every erase
// and program.
#pragma once
#include <stdint.h>

#include <vector>

#include "jl.h"

namespace fake {

static constexpr uint32_t SIZE = 0x100000;
extern uint8_t flash[SIZE];
struct Op {
    char kind;  // 'e' erase, 'p' program
    uint32_t addr, len;
};
extern std::vector<Op> ops;
extern int fail_reads;  // the next n jl::read calls fail

void reset();
// Whether any erase or program touched [lo, hi).
bool touched(uint32_t lo, uint32_t hi);
// The sectors erased, in order.
std::vector<uint32_t> erased_sectors();

}  // namespace fake
