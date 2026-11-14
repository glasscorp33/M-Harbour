#include "ui.h"

#include <stdarg.h>

#include "art.h"
#include "sd.h"
#include "ui_fonts.h"

static constexpr gpio_num_t PIN_G0 = GPIO_NUM_0;  // G0 button, active low

// ---------------------------------------------------------------------------
// Log and console

static constexpr int LOG_LINES = 9;
static char log_buf[LOG_LINES][41];
static int log_n;
static const char *con_title = "";
static uint16_t con_color = TFT_WHITE;
static char con_status[41];
static bool con_visible;  // the console is what the screen shows

static void draw_console() {
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(2);
    lcd.setTextColor(con_color, TFT_BLACK);
    lcd.setCursor(4, 2);
    lcd.print(con_title);
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    int first = log_n > LOG_LINES ? log_n - LOG_LINES : 0;
    for (int i = first; i < log_n; i++) {
        lcd.setCursor(2, 22 + (i - first) * 10);
        lcd.print(log_buf[i % LOG_LINES]);
    }
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(2, 124);
    lcd.print(con_status);
    lcd.endWrite();
}

void ulog(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (sd_ok) {
        File f = SD.open("/fm1/log.txt", FILE_APPEND);
        if (f) {
            f.printf("%9lu %s\n", (unsigned long)millis(), line);
            f.close();
        }
    }
    for (size_t off = 0, n = strlen(line); off < n || off == 0; off += 40) {
        snprintf(log_buf[log_n % LOG_LINES], 41, "%s", line + off);
        log_n++;
        if (n <= off + 40) break;
    }
    if (con_visible) draw_console();
}

namespace ui {

void console(const char *title, uint16_t color) {
    con_title = title;
    con_color = color;
    con_visible = true;
    draw_console();
}

void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(con_status, sizeof con_status, fmt, ap);
    va_end(ap);
    if (con_visible) draw_console();
}

void clear_log() { log_n = 0; }

void show_console() {
    con_title = "Details";
    con_color = TFT_LIGHTGREY;
    snprintf(con_status, sizeof con_status, "Any key: back");
    con_visible = true;
    draw_console();
}

// ---------------------------------------------------------------------------
// Keys

bool g0_pressed() { return gpio_get_level(PIN_G0) == 0; }

int read_key() {
    static bool g0_was;
    bool g0 = g0_pressed();
    bool g0_edge = g0 && !g0_was;
    g0_was = g0;
    if (g0_edge) return '\n';
    M5Cardputer.update();
    auto &kb = M5Cardputer.Keyboard;
    if (!kb.isChange() || !kb.isPressed()) return 0;
    auto st = kb.keysState();
    if (st.enter) return '\n';
    if (st.del) return 27;
    if (!st.word.empty()) {
        char c = st.word[0];
        if (c == '`') return 27;
        return tolower(c);
    }
    return 0;
}

// read_input's memory of what is held (see there)
static std::vector<char> in_prev_word;
static bool in_prev[8];

void wait_released() {
    do {
        delay(20);
        M5Cardputer.update();
    } while (g0_pressed() || M5Cardputer.Keyboard.isPressed());
    // nothing is held now, whoever saw the keys go up
    in_prev_word.clear();
    memset(in_prev, 0, sizeof in_prev);
}

bool any_key_cancel() {
    if (g0_pressed()) return true;
    M5Cardputer.update();
    return M5Cardputer.Keyboard.isPressed() > 0;
}

int read_input() {
    static bool g0_was;
    bool g0 = g0_pressed();
    bool g0_edge = g0 && !g0_was;
    g0_was = g0;
    if (g0_edge) return '\n';
    // what was down before: only what goes down now counts
    M5Cardputer.update();
    auto &kb = M5Cardputer.Keyboard;
    if (!kb.isChange()) return 0;
    auto st = kb.keysState();
    bool now[8] = {st.enter, st.backspace, st.del, st.esc, st.up, st.down, st.left, st.right};
    bool was[8];
    memcpy(was, in_prev, sizeof was);
    memcpy(in_prev, now, sizeof in_prev);
    std::vector<char> old = in_prev_word;
    in_prev_word = st.word;
    static const int codes[8] = {'\n', IN_BKSP, IN_DEL, IN_ESC, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT};
    for (int i = 0; i < 8; i++)
        if (now[i] && !was[i]) return codes[i];
    for (char c : st.word) {
        bool held = false;
        for (char o : old) held |= o == c;
        if (held) continue;
        if (c == '`') return IN_ESC;
        if (c >= 0x20 && c < 0x7F) return c;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The name window and the list screen (Font0: 6x8 at size 1, 12x16 at 2)

static void small(int x, int y, uint16_t color, const char *s) {
    lcd.setTextSize(1);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.setCursor(x, y);
    lcd.print(s);
}

static void draw_input(const char *title, const char *help1, const char *help2, const char *buf, size_t cap) {
    static constexpr int PER_LINE = 19, BOX_Y = 42;
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(2);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setCursor(4, 2);
    lcd.print(title);
    small(2, 22, TFT_LIGHTGREY, help1 ? help1 : "");
    small(2, 31, TFT_LIGHTGREY, help2 ? help2 : "");
    lcd.drawRoundRect(0, BOX_Y, 240, 40, 3, TFT_DARKGREY);
    lcd.setTextSize(2);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    size_t n = strlen(buf);
    for (int line = 0; line < 2; line++) {
        char part[PER_LINE + 1] = "";
        size_t from = line * PER_LINE;
        if (from < n) snprintf(part, sizeof part, "%.*s", PER_LINE, buf + from);
        lcd.setCursor(6, BOX_Y + 4 + line * 17);
        lcd.print(part);
    }
    // the cursor after the last character
    size_t at = n < 2 * PER_LINE ? n : 2 * PER_LINE - 1;
    lcd.fillRect(6 + (at % PER_LINE) * 12, BOX_Y + 4 + (at / PER_LINE) * 17 + 15, 10, 2, TFT_YELLOW);
    char count[16];
    snprintf(count, sizeof count, "%u/%u", (unsigned)n, (unsigned)(cap - 1));
    small(240 - 6 * (int)strlen(count) - 2, BOX_Y + 42, TFT_DARKGREY, count);
    small(2, 114, TFT_YELLOW, "ENTER ok  BKSP delete  ESC cancel");
    small(2, 124, TFT_YELLOW, "shift: capitals, ! @ #   fn+BKSP clear");
    lcd.endWrite();
}

bool text_input(const char *title, const char *help1, const char *help2, char *buf, size_t cap, bool (*tick)()) {
    con_visible = false;
    char work[64];
    if (cap > sizeof work) cap = sizeof work;
    snprintf(work, cap, "%s", buf);
    draw_input(title, help1, help2, work, cap);
    wait_released();
    while (read_input()) {}  // what was held before does not count
    uint32_t last_tick = millis();
    for (;;) {
        int k = read_input();
        size_t n = strlen(work);
        bool redraw = true;
        if (k == '\n') {
            if (n && strspn(work, " ") != n) {
                snprintf(buf, cap, "%s", work);
                wait_released();
                return true;
            }
            redraw = false;
        } else if (k == IN_ESC) {
            wait_released();
            return false;
        } else if (k == IN_BKSP) {
            if (n) work[n - 1] = 0;
        } else if (k == IN_DEL) {
            work[0] = 0;
        } else if (k >= 0x20 && k < 0x7F) {
            if (n + 1 < cap) {
                work[n] = (char)k;
                work[n + 1] = 0;
            }
        } else {
            redraw = false;
        }
        if (redraw) draw_input(title, help1, help2, work, cap);
        delay(15);
        if (tick && millis() - last_tick > 1000) {
            last_tick = millis();
            if (!tick()) return false;
        }
    }
}

void list_screen(const char *title, uint16_t color, const char *subtitle, const Row *rows, int n, int first,
                 int selected, const char *detail, const char *help1, const char *help2) {
    con_visible = false;
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(2);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.setCursor(4, 2);
    lcd.print(title);
    int tx = 4 + 12 * (int)strlen(title) + 8;
    if (subtitle) {
        char sub[41];
        int room = (240 - tx) / 6;
        snprintf(sub, sizeof sub, "%.*s", room > 0 && room < 40 ? room : 0, subtitle);
        small(tx, 7, TFT_LIGHTGREY, sub);
    }
    for (int i = 0; i < LIST_ROWS && first + i < n; i++) {
        const Row &r = rows[first + i];
        int y = 22 + i * 10;
        bool sel = first + i == selected;
        if (sel) lcd.fillRect(0, y - 1, 240, 10, 0x2945);
        lcd.setTextSize(1);
        lcd.setTextColor(sel ? TFT_WHITE : r.color, sel ? 0x2945 : TFT_BLACK);
        lcd.setCursor(2, y);
        lcd.print(r.text);
    }
    if (n > LIST_ROWS) {  // where in the list
        int h = 80 * LIST_ROWS / n, y = 21 + 80 * first / n;
        lcd.fillRect(237, 21, 3, 80, 0x2104);
        lcd.fillRect(237, y, 3, h < 4 ? 4 : h, TFT_DARKGREY);
    }
    small(2, 103, TFT_LIGHTGREY, detail ? detail : "");
    small(2, 114, TFT_YELLOW, help1 ? help1 : "");
    small(2, 124, TFT_YELLOW, help2 ? help2 : "");
    lcd.endWrite();
}

// ---------------------------------------------------------------------------
// Guided pages
//
//  +------------+-----------------------------+
//  |            | Title (bold, accent)        |  240 x 135
//  |  picture   | body text, word-wrapped     |
//  |  100x100   | [progress bar]              |
//  |            |                             |
//  +------------+-----------------------------+
//  | [KEY] what it does                       |  footer
//  +------------------------------------------+

static constexpr int ART_X = 2, ART_Y = 6, ART_W = 100, ART_H = 100;
static constexpr int TEXT_X = 106, TEXT_W = 240 - 106 - 2, TEXT_Y = 4;
static constexpr int FOOT_Y = 113, FOOT_H = 135 - 113;
// the pages' fonts (src/ui_fonts.h: DejaVu Sans, permissive) and line pitches
static const lgfx::IFont *const TITLE = &UiTitle, *const BODY = &UiBody;
static constexpr int TITLE_LINE = 18, LINE = 16;
static int text_end_y;           // where the page's words ended
static uint16_t page_accent;

// Draws s word-wrapped in [x, x + w) from top y, `pitch` apart; returns the next y.
static int wrap(const char *s, int x, int y, int w, int pitch = LINE) {
    char line[64], word[40];
    int ll = 0;
    line[0] = 0;
    auto flush = [&]() {
        if (ll) {
            lcd.drawString(line, x, y);
            y += pitch;
        }
        ll = 0;
        line[0] = 0;
    };
    while (*s) {
        if (*s == '\n') {
            flush();
            s++;
            continue;
        }
        int wl = 0;
        while (*s && *s != ' ' && *s != '\n' && wl < (int)sizeof word - 1) word[wl++] = *s++;
        word[wl] = 0;
        while (*s == ' ') s++;
        char trial[64];
        snprintf(trial, sizeof trial, ll ? "%s %s" : "%s%s", line, word);
        if (ll && lcd.textWidth(trial) > w) {
            flush();
            snprintf(trial, sizeof trial, "%s", word);
        }
        snprintf(line, sizeof line, "%s", trial);
        ll = strlen(line);
    }
    flush();
    return y;
}

void footer(uint16_t color, const char *text) {
    lcd.fillRect(0, FOOT_Y, 240, FOOT_H, TFT_BLACK);
    lcd.drawFastHLine(0, FOOT_Y, 240, 0x2104);
    lcd.setFont(BODY);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.setTextDatum(middle_left);
    lcd.drawString(text, 6, FOOT_Y + FOOT_H / 2 + 1);
    lcd.setTextDatum(top_left);
}

static void key_footer(const char *key, const char *hint, const char *right) {
    lcd.fillRect(0, FOOT_Y, 240, FOOT_H, TFT_BLACK);
    lcd.drawFastHLine(0, FOOT_Y, 240, 0x2104);
    lcd.setFont(TITLE);
    int kw = lcd.textWidth(key) + 12;
    if (kw < 22) kw = 22;
    // the key cap
    lcd.fillRoundRect(4, FOOT_Y + 3, kw, FOOT_H - 5, 4, page_accent);
    lcd.setTextColor(TFT_BLACK, page_accent);
    lcd.setTextDatum(middle_center);
    lcd.drawString(key, 4 + kw / 2, FOOT_Y + FOOT_H / 2 + 1);
    if (hint) {
        lcd.setFont(BODY);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setTextDatum(middle_left);
        lcd.drawString(hint, 4 + kw + 6, FOOT_Y + FOOT_H / 2 + 1);
    }
    if (right) {
        lcd.setFont(BODY);
        lcd.setTextColor(GREY, TFT_BLACK);
        lcd.setTextDatum(middle_right);
        lcd.drawString(right, 236, FOOT_Y + FOOT_H / 2 + 1);
    }
    lcd.setTextDatum(top_left);
}

void page(const char *art_name, uint16_t accent, const char *title, const char *body, const char *key,
          const char *hint, const char *right) {
    con_visible = false;
    page_accent = accent;
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    art::draw(art_name, ART_X, ART_Y, ART_W, ART_H, accent);
    lcd.setTextDatum(top_left);
    lcd.setFont(TITLE);
    lcd.setTextColor(accent, TFT_BLACK);
    int y = wrap(title, TEXT_X, TEXT_Y, TEXT_W, TITLE_LINE);
    lcd.setFont(BODY);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    text_end_y = wrap(body ? body : "", TEXT_X, y + 2, TEXT_W);
    if (key) key_footer(key, hint, right);
    else if (hint) footer(TFT_LIGHTGREY, hint);
    lcd.endWrite();
}

void note(uint16_t color, const char *text) {
    int y = text_end_y + 2;
    lcd.startWrite();
    lcd.fillRect(TEXT_X, y, TEXT_W, LINE + 2, TFT_BLACK);
    lcd.setFont(TITLE);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.drawString(text, TEXT_X, y);
    lcd.endWrite();
}

void progress(uint32_t done, uint32_t total) {
    int y = text_end_y + 4, w = TEXT_W, h = 12;
    if (y + h + 16 > FOOT_Y) y = FOOT_Y - h - 18;
    uint32_t pct = total ? done * 100 / total : 0;
    if (pct > 100) pct = 100;
    lcd.startWrite();
    lcd.drawRoundRect(TEXT_X, y, w, h, 3, TFT_DARKGREY);
    lcd.fillRoundRect(TEXT_X + 2, y + 2, (w - 4) * pct / 100, h - 4, 2, page_accent);
    lcd.fillRect(TEXT_X, y + h + 2, w, 16, TFT_BLACK);
    lcd.setFont(BODY);
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char s[8];
    snprintf(s, sizeof s, "%lu%%", (unsigned long)pct);
    lcd.drawString(s, TEXT_X, y + h + 2);
    lcd.endWrite();
}

void spinner() {
    static uint32_t last;
    static int phase;
    if (millis() - last < 250) return;
    last = millis();
    phase = (phase + 1) % 4;
    int y = text_end_y + 10;
    if (y > FOOT_Y - 8) y = FOOT_Y - 8;
    lcd.startWrite();
    for (int i = 0; i < 3; i++)
        lcd.fillCircle(TEXT_X + 6 + i * 14, y, 4, i < phase ? page_accent : 0x2104);
    lcd.endWrite();
}

}  // namespace ui
