// The firmware manager (docs/FIRMWARE-MANAGER.md; screens as in
// docs/mockup/firmware-manager-ui.html): connect without the power-on key
// (the soft key over USB MIDI, by itself), then BACKUP CURRENT, LIBRARY
// (folders), LOAD FIRMWARE, INFO, exit.
#pragma once

namespace fwman {
// Returns only from the "connect" screen (ESC), before the USB host is
// started; after that a session ends with a restart of the Cardputer.
void run();
}
