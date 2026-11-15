// The guided mode: one step a screen, a picture, a few words, one key.
#pragma once

namespace guided {
// switch_on: start at "switch it on" (a restart after "try again").
// Returns on ESC on its first page (back to the home menu).
void run(bool switch_on);
}
