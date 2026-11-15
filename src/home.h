// The home menu: Recovery (guided, advanced) or Firmware manager.
#pragma once
#include "modes.h"

namespace home {
// m: what the Cardputer was restarted into (modes::at_boot()). Never returns.
[[noreturn]] void run(modes::Mode m);
}
