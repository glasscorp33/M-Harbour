// Which mode runs, across a restart of the Cardputer: the USB host can only be
// let go of by restarting, so "try again" after a connection restarts into
// the step it came from.
#pragma once
#include <stdint.h>

namespace modes {

// HOME: the home menu (Recovery, Firmware manager); FW_MANAGER: the
// firmware manager's "connect" screen; the others: the recovery's modes.
enum Mode : uint32_t { GUIDED = 1, ADVANCED = 2, GUIDED_SWITCH_ON = 3, HOME = 4, FW_MANAGER = 5 };

[[noreturn]] void restart_into(Mode m);
// What restart_into asked for; HOME after a power-on.
Mode at_boot();

}  // namespace modes
