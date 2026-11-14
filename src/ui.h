// Screen and keys: the log (to /fm1/log.txt and the advanced mode's console)
// and the guided mode's pages (a picture, a few large words, the key to press).
#pragma once
#include <M5Cardputer.h>

// Not a global reference: M5Cardputer lives in another file and may not be
// constructed yet when this file's globals are initialized.
#define lcd (M5Cardputer.Display)

// One line in /fm1/log.txt and the console's buffer (drawn only while the
// console is on screen).
void ulog(const char *fmt, ...);

namespace ui {

// colours of the guided pages
static constexpr uint16_t CYAN = 0x065F, GREEN = 0x2F2D, ORANGE = 0xFCE3, RED = 0xF9C6, YELLOW = 0xFEA1, GREY = 0x7BEF;

// --- keys
bool g0_pressed();
// '\n' ENTER or G0, 27 ESC (` or DEL), a lower-case letter or digit, or 0.
int read_key();
void wait_released();
bool any_key_cancel();

// For typing and lists: each key once, as it goes down (a key still held
// while another is pressed is not repeated), with its case and shifted
// symbol; or one of these. 0: nothing new.
enum : int { IN_BKSP = 8, IN_ESC = 27, IN_DEL = 0x7F, IN_UP = 0x101, IN_DOWN, IN_LEFT, IN_RIGHT };
// '\n' ENTER or G0, IN_ESC (` or fn+`), IN_BKSP, IN_DEL (fn+BKSP), the
// arrows (fn+; . , /), or a printable character (shift: capitals, ! @ # ...).
int read_input();

// The name window: a title, two lines of help, the text being typed
// (prefilled with buf), at most cap - 1 characters (printable ASCII).
// ENTER keeps it (not if empty), ESC cancels, BKSP deletes the last
// character, fn+BKSP clears it all. tick() runs about once a second and
// returns false when the unit is gone (cancelled then). True: buf is the name.
bool text_input(const char *title, const char *help1, const char *help2, char *buf, size_t cap, bool (*tick)());

// A list screen: a title (and a smaller subtitle), up to LIST_ROWS rows from
// `first` with `selected` highlighted, a detail line, two help lines.
static constexpr int LIST_ROWS = 8;
struct Row {
    char text[41];
    uint16_t color;
};
void list_screen(const char *title, uint16_t color, const char *subtitle, const Row *rows, int n, int first,
                 int selected, const char *detail, const char *help1, const char *help2);

// --- the advanced mode's console: a title, the last log lines, a status line
void console(const char *title, uint16_t color);
void status(const char *fmt, ...);
void clear_log();
void show_console();  // the log as it is, for "details" from the guided mode

// --- guided pages: the picture on the left, the words on the right, and at
// the bottom the key to press (key nullptr: none), what it does, and a small
// grey label on the right (another key: "esc", "A: advanced").
void page(const char *art, uint16_t accent, const char *title, const char *body, const char *key = nullptr,
          const char *hint = nullptr, const char *right = "esc");
// A line in colour under the page's words (e.g. "Card found").
void note(uint16_t color, const char *text);
// A bar under the page's words (done of total).
void progress(uint32_t done, uint32_t total);
// The page's bottom line, replaced (e.g. a live state).
void footer(uint16_t color, const char *text);
// Three dots that move: call it often while waiting.
void spinner();

}  // namespace ui
