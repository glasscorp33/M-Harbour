// The home menu: RECOVERY (the guided and advanced modes, as they were) or
// FIRMWARE MANAGER (fwman.cpp). Screens as in docs/mockup/.
#include "home.h"

#include "advanced.h"
#include "fm_ui.h"
#include "fwman.h"
#include "guided.h"

namespace home {

using namespace fmui;

// Two big boxes; 0 or 1 chosen, -1 on ESC (when back is allowed).
static int two_boxes(const char *title, uint16_t color, const Box *b, int sel, bool back) {
    ui::wait_released();
    for (;;) {
        begin(title, color);
        boxes(BODY_Y, b, 2, sel, 40);
        foot(back ? G_UP G_DOWN " move   ENTER choose   ESC back" : G_UP G_DOWN " move   ENTER choose   1-2 shortcut");
        end();
        int k = 0;
        while (!(k = nav(ui::read_input()))) delay(15);
        if (move(k, &sel, 2)) continue;
        if (k == '1' || k == '2') return k - '1';
        if (k == '\n') return sel;
        if (k == K_BACK && back) return -1;
    }
}

// RECOVERY: GUIDED or ADVANCED, until ESC.
static void recovery(int sel) {
    static const Box b[] = {{"GUIDED", "Step by step, for a dead device"},
                            {"ADVANCED", "Backup, restore, repair, tests"}};
    for (;;) {
        int c = two_boxes("RECOVERY", REC, b, sel, true);
        if (c < 0) return;
        sel = c;
        ui::wait_released();
        if (c == 0) guided::run(false);  // back here on ESC on its first page
        else advanced::run();            // back here on ESC on its first screen
    }
}

void run(modes::Mode m) {
    // after a restart into one of the recovery's modes: there first
    if (m == modes::GUIDED || m == modes::GUIDED_SWITCH_ON) {
        guided::run(m == modes::GUIDED_SWITCH_ON);
        recovery(0);
    } else if (m == modes::ADVANCED) {
        advanced::run();
        recovery(1);
    } else if (m == modes::FW_MANAGER) {
        fwman::run();
    }
    static const Box b[] = {{"RECOVERY", "Fix a device that does not start (brick)"},
                            {"FIRMWARE MANAGER", "Back up, keep and switch firmwares"}};
    char title[32];
    snprintf(title, sizeof title, "SYNTH TOOL  %s", APP_VERSION);
    int sel = m == modes::FW_MANAGER ? 1 : 0;
    for (;;) {
        sel = two_boxes(title, HOME, b, sel, false);
        ui::wait_released();
        if (sel == 0) recovery(0);
        else fwman::run();  // returns only before it has started the USB host
    }
}

}  // namespace home
