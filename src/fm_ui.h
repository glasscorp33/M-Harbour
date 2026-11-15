// The screens of the home menu and the firmware manager, drawn as in the
// approved mockup (docs/mockup/firmware-manager-ui.html) at 240 x 135: a 15 px
// title bar in the section's colour with the battery, the body, an 11 px
// yellow key-hint footer; big boxed menu items, lists of rows, yes/no boxes.
//
// A few signs are drawn, not taken from a font (the fonts are ASCII): put
// these in a string (as separate literals: G_CHECK "Saved").
#pragma once
#include <stdint.h>

#include "ui.h"

#define G_CHECK "\x01"  // a tick
#define G_DOT "\x02"    // a filled circle (on the device now)
#define G_MID "\x03"    // a middle dot (separator)
#define G_TRI "\x04"    // a small triangle (a folder)
#define G_UP "\x05"
#define G_DOWN "\x06"
#define G_LEFT "\x0e"
#define G_RIGHT "\x0f"

namespace fmui {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// the sections' title bars
static constexpr uint16_t HOME = rgb(0xff, 0xd8, 0x4a), REC = rgb(0xff, 0x8a, 0x5a), FM = rgb(0x5f, 0xd7, 0xff),
                          BACKUP = rgb(0x7c, 0xf0, 0x8a), LIB = rgb(0xc9, 0xa7, 0xff), LOAD = rgb(0xff, 0xb0, 0x2e),
                          INFO = rgb(0xbb, 0xbb, 0xbb);
// text
static constexpr uint16_t GOOD = rgb(0x59, 0xe3, 0x6b), WARN = rgb(0xff, 0xb0, 0x2e), BAD = rgb(0xff, 0x5a, 0x5a),
                          DIM = rgb(0x88, 0x88, 0x88), CY = rgb(0x5f, 0xd7, 0xff), FG = 0xFFFF, BG = 0;

enum Font { BAR, BOX, MSG, TEXT, SMALL };  // bold 10, bold 13, 11, 10, the 6x8 pixel font

// Layout
static constexpr int W = 240, H = 135, BAR_H = 15, FOOT_H = 11, BODY_Y = BAR_H + 3, FOOT_Y = H - FOOT_H, X = 4;

// The screen cleared, the title bar (title, battery unless !battery); call
// end() when drawn.
void begin(const char *title, uint16_t color, bool battery = true);
void end();
void foot(const char *keys);

// Text with its vertical centre at cy; maxw > 0: cut to fit with "..".
// Returns the x after it.
int text(int x, int cy, const char *s, Font f, uint16_t color, int maxw = 0);
int width(const char *s, Font f);
// A line of the body: top y, height h, at X, cut to the screen's width.
void line(int y, int h, Font f, uint16_t color, const char *s);

// Menu boxes from top y, each h high (27; 40 with a subtitle under the
// title), 3 apart, numbered 1, 2, ...
struct Box {
    const char *title, *sub;
};
void boxes(int y, const Box *b, int n, int sel, int h = 27);
// Two boxes side by side (DELETE / KEEP, LOAD / CANCEL), 24 high.
void yesno(int y, const char *a, const char *b, int sel);

// List rows from top y, rh high, vis of them shown around sel.
struct Row {
    const char *ic;   // the sign in front: G_TRI, "+", "/", G_DOT or ""
    char t[48];       // the text
    char r[24];       // grey, on the right
    uint16_t color;   // of t (white when 0)
};
// first >= 0: that row first (sel may then be -1: none highlighted).
void rows(int y, const Row *r, int n, int sel, int vis = 6, int rh = 13, Font f = TEXT, int first = -1);
// The first row shown for sel (the same as rows() draws).
int first_row(int n, int sel, int vis);

// Text word-wrapped to the screen's width from top y, lh apart, at most
// max_lines; returns the y after it.
int para(int y, int lh, Font f, uint16_t color, const char *s, int max_lines = 4);
// Key and value (Device  SMK-37), top y, 12 high.
void kv(int y, const char *k, const char *v, uint16_t vcolor = FG);
// A bar: done of total, top y.
void progress(int y, uint32_t done, uint32_t total, uint16_t color = GOOD);
// The name field, top y, 20 high, the end of s shown if it is long; cursor on/off.
void input(int y, const char *s, bool cursor);

// The Cardputer's battery in % (-1: no believable reading).
int battery_percent();

// --- keys on the menus and lists (not the name field, where ; . , / are typed)
enum : int { K_UP = ui::IN_UP, K_DOWN = ui::IN_DOWN, K_LEFT = ui::IN_LEFT, K_RIGHT = ui::IN_RIGHT, K_BACK = 27 };
// A key from ui::read_input() for a menu: ; . , / (with or without fn) the
// arrows, ESC (`), BKSP and fn+BKSP back (K_BACK), letters lower-case;
// ENTER '\n'; 0 for nothing.
int nav(int k);
// Up/down (or with horizontal, also left/right) moves sel in [0, n), round.
bool move(int k, int *sel, int n, bool horizontal = false);

}  // namespace fmui
