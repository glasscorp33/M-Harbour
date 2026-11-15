#include "fm_ui.h"

#include <string.h>

#include "library.h"
#include "ui_fonts.h"

namespace fmui {

static const lgfx::IFont *font_of(Font f) {
    switch (f) {
        case BAR: return &FmBar;
        case BOX: return &FmBox;
        case MSG: return &FmMsg;
        case TEXT: return &FmText;
        default: return &fonts::Font0;
    }
}

static void use(Font f) {
    lcd.setFont(font_of(f));
    lcd.setTextSize(1);
}

// The drawn signs: how wide (with the space after them) for a font h high.
static bool is_sign(char c) { return c >= 1 && c <= 0x0f && c != '\n'; }

static int sign_w(char c, int h) {
    switch (c) {
        case 1: return h * 2 / 3 + 3;  // tick
        case 2: return (h / 4 > 2 ? h / 4 : 2) * 2 + 4;
        case 3: return 5;
        case 4: return 7;
        default: return 9;  // arrows
    }
}

static void sign(char c, int x, int cy, int h, uint16_t color) {
    switch (c) {
        case 1: {  // tick
            int s = h * 2 / 3;
            for (int t = 0; t < 2; t++) {
                lcd.drawLine(x, cy - t, x + s / 3, cy + s / 3 - t, color);
                lcd.drawLine(x + s / 3, cy + s / 3 - t, x + s, cy - s / 2 - t, color);
            }
            break;
        }
        case 2: {
            int r = h / 4 > 2 ? h / 4 : 2;
            lcd.fillCircle(x + r + 1, cy, r, color);
            break;
        }
        case 3: lcd.fillRect(x + 1, cy - 1, 2, 2, color); break;
        case 4: lcd.fillTriangle(x, cy - 3, x, cy + 3, x + 4, cy, color); break;
        case 5:  // up
            lcd.fillTriangle(x, cy - 1, x + 6, cy - 1, x + 3, cy - 4, color);
            lcd.drawFastVLine(x + 3, cy - 1, 5, color);
            break;
        case 6:  // down
            lcd.fillTriangle(x, cy + 1, x + 6, cy + 1, x + 3, cy + 4, color);
            lcd.drawFastVLine(x + 3, cy - 3, 5, color);
            break;
        case 0x0e:  // left
            lcd.fillTriangle(x + 3, cy - 3, x + 3, cy + 3, x, cy, color);
            lcd.drawFastHLine(x + 3, cy, 4, color);
            break;
        case 0x0f:  // right
            lcd.fillTriangle(x + 4, cy - 3, x + 4, cy + 3, x + 7, cy, color);
            lcd.drawFastHLine(x, cy, 4, color);
            break;
        default: break;
    }
}

// Width of s in the font already set.
static int measure(const char *s) {
    int h = lcd.fontHeight(), w = 0;
    char seg[64];
    while (*s) {
        if (is_sign(*s)) {
            w += sign_w(*s++, h);
            continue;
        }
        size_t n = 0;
        while (s[n] && !is_sign(s[n]) && n < sizeof seg - 1) n++;
        memcpy(seg, s, n);
        seg[n] = 0;
        w += lcd.textWidth(seg);
        s += n;
    }
    return w;
}

int width(const char *s, Font f) {
    use(f);
    return measure(s);
}

static int draw(int x, int cy, const char *s, uint16_t color) {
    int h = lcd.fontHeight();
    char seg[64];
    lcd.setTextColor(color);
    lcd.setTextDatum(middle_left);
    while (*s) {
        if (is_sign(*s)) {
            sign(*s, x, cy, h, color);
            x += sign_w(*s++, h);
            continue;
        }
        size_t n = 0;
        while (s[n] && !is_sign(s[n]) && n < sizeof seg - 1) n++;
        memcpy(seg, s, n);
        seg[n] = 0;
        x += lcd.drawString(seg, x, cy);
        s += n;
    }
    lcd.setTextDatum(top_left);
    return x;
}

int text(int x, int cy, const char *s, Font f, uint16_t color, int maxw) {
    use(f);
    if (maxw <= 0 || measure(s) <= maxw) return draw(x, cy, s, color);
    char cut[96];
    size_t n = strlen(s);
    if (n > sizeof cut - 3) n = sizeof cut - 3;
    for (; n > 0; n--) {
        memcpy(cut, s, n);
        while (n && cut[n - 1] == ' ') n--;
        memcpy(cut + n, "..", 3);
        if (measure(cut) <= maxw) break;
    }
    if (!n) snprintf(cut, sizeof cut, "..");
    return draw(x, cy, cut, color);
}

void line(int y, int h, Font f, uint16_t color, const char *s) { text(X, y + h / 2, s, f, color, W - 2 * X); }

int battery_percent() {
    int pct;
    library::battery(M5.Power.getBatteryVoltage(), &pct);
    return pct;
}

void begin(const char *title, uint16_t color, bool battery) {
    lcd.startWrite();
    lcd.fillScreen(BG);
    lcd.fillRect(0, 0, W, BAR_H, color);
    int pct = battery ? battery_percent() : -1;
    int right = W - 4;
    if (pct >= 0) {
        char s[12];
        snprintf(s, sizeof s, "%d%%", pct);
        use(SMALL);
        int tw = lcd.textWidth(s);
        draw(right - tw, BAR_H / 2, s, BG);
        right -= tw + 3;
        // a small battery
        lcd.drawRect(right - 11, 4, 11, 7, BG);
        lcd.fillRect(right - 1, 6, 2, 3, BG);
        lcd.fillRect(right - 10, 5, 9 * pct / 100, 5, BG);
        right -= 15;
    }
    text(X, BAR_H / 2, title, BAR, BG, right - X);
}

void end() { lcd.endWrite(); }

void foot(const char *keys) {
    lcd.fillRect(0, FOOT_Y, W, FOOT_H, rgb(0x11, 0x11, 0x11));
    text(X, FOOT_Y + FOOT_H / 2, keys, SMALL, HOME, W - 2 * X);
}

void boxes(int y, const Box *b, int n, int sel, int h) {
    for (int i = 0; i < n; i++, y += h + 3) {
        bool on = i == sel;
        if (on) lcd.fillRoundRect(X, y, W - 2 * X, h, 3, FG);
        else lcd.drawRoundRect(X, y, W - 2 * X, h, 3, rgb(0x55, 0x55, 0x55));
        char num[12];
        snprintf(num, sizeof num, "%d", i + 1);
        text(X + 7, y + h / 2, num, SMALL, on ? rgb(0x55, 0x55, 0x55) : DIM);
        int tx = X + 21, tw = W - 2 * X - 25;
        bool sub = b[i].sub && b[i].sub[0];
        if (!sub) {
            text(tx, y + h / 2, b[i].title, BOX, on ? BG : FG, tw);
        } else {
            int th = 14, sh = 11, top = y + (h - th - sh) / 2;
            text(tx, top + th / 2, b[i].title, BOX, on ? BG : FG, tw);
            text(tx, top + th + sh / 2, b[i].sub, TEXT, on ? rgb(0x33, 0x33, 0x33) : rgb(0xaa, 0xaa, 0xaa), tw);
        }
    }
}

void yesno(int y, const char *a, const char *b, int sel) {
    int w = (W - 2 * X - 6) / 2;
    for (int i = 0; i < 2; i++) {
        int x = X + i * (w + 6);
        bool on = i == sel;
        if (on) lcd.fillRoundRect(x, y, w, 24, 3, FG);
        else lcd.drawRoundRect(x, y, w, 24, 3, rgb(0x55, 0x55, 0x55));
        const char *t = i ? b : a;
        int tw = width(t, BAR);
        text(x + (w - tw) / 2, y + 12, t, BAR, on ? BG : FG, w - 4);
    }
}

int first_row(int n, int sel, int vis) {
    int first = sel - 2;
    if (first > n - vis) first = n - vis;
    return first < 0 ? 0 : first;
}

void rows(int y, const Row *r, int n, int sel, int vis, int rh, Font f, int first) {
    if (first < 0) first = first_row(n, sel, vis);
    for (int k = 0; k < vis && first + k < n; k++) {
        const Row &row = r[first + k];
        int ry = y + k * rh, cy = ry + rh / 2;
        bool on = first + k == sel;
        if (on) lcd.fillRect(X - 1, ry, W - 2 * X + 2, rh, FG);
        uint16_t fg = on ? BG : row.color ? row.color : FG;
        int x = X + 2;
        if (row.ic && row.ic[0]) text(x, cy, row.ic, f, on ? BG : row.ic[0] == '\x02' ? GOOD : fg);
        x += 11;
        int rw = row.r[0] ? width(row.r, SMALL) + 4 : 0;
        text(x, cy, row.t, f, fg, W - X - 2 - x - rw);
        if (rw) text(W - X - 2 - rw + 4, cy, row.r, SMALL, on ? rgb(0x44, 0x44, 0x44) : DIM);
    }
    if (n > vis) {  // where in the list
        int track = vis * rh, bar = track * vis / n, at = track * first / n;
        lcd.fillRect(W - 2, y, 2, track, rgb(0x22, 0x22, 0x22));
        lcd.fillRect(W - 2, y + at, 2, bar < 4 ? 4 : bar, DIM);
    }
}

int para(int y, int lh, Font f, uint16_t color, const char *s, int max_lines) {
    char ln[96];
    int lines = 0;
    while (*s && lines < max_lines) {
        while (*s == ' ') s++;
        // the longest run of words that fits (a word too long is cut)
        size_t best = 0, n = 0;
        for (;;) {
            while (s[n] && s[n] != ' ' && n < sizeof ln - 1) n++;
            memcpy(ln, s, n);
            ln[n] = 0;
            if (best && width(ln, f) > W - 2 * X) break;
            best = n;
            if (!s[n] || n >= sizeof ln - 1) break;
            n++;
        }
        memcpy(ln, s, best);
        ln[best] = 0;
        bool last = ++lines == max_lines && s[best];
        text(X, y + lh / 2, ln, f, color, last ? W - 2 * X - 1 : W - 2 * X);
        y += lh;
        s += best;
    }
    return y;
}

void kv(int y, const char *k, const char *v, uint16_t vcolor) {
    text(X, y + 6, k, TEXT, DIM);
    text(X + 62, y + 6, v, TEXT, vcolor, W - X - 62 - X);
}

void progress(int y, uint32_t done, uint32_t total, uint16_t color) {
    uint32_t pct = total ? (uint64_t)done * 100 / total : 0;
    if (pct > 100) pct = 100;
    lcd.startWrite();
    lcd.drawRect(X, y, W - 2 * X, 10, DIM);
    lcd.fillRect(X + 1, y + 1, (W - 2 * X - 2) * pct / 100, 8, color);
    lcd.endWrite();
}

void input(int y, const char *s, bool cursor) {
    lcd.fillRect(X, y, W - 2 * X, 20, BG);
    lcd.drawRect(X, y, W - 2 * X, 20, FG);
    int room = W - 2 * X - 8 - 7;
    const char *show = s;
    while (*show && width(show, MSG) > room) show++;  // the end of a long name
    int x = text(X + 4, y + 10, show, MSG, FG);
    lcd.fillRect(x + 1, y + 4, 6, 12, cursor ? FG : BG);
}

int nav(int k) {
    switch (k) {
        case ';': return K_UP;
        case '.': return K_DOWN;
        case ',': return K_LEFT;
        case '/': return K_RIGHT;
        case ui::IN_ESC:
        case ui::IN_BKSP:
        case ui::IN_DEL: return K_BACK;
        default: break;
    }
    if (k >= 'A' && k <= 'Z') return k - 'A' + 'a';
    return k;
}

bool move(int k, int *sel, int n, bool horizontal) {
    if (n <= 0) return false;
    bool prev = k == K_UP || (horizontal && k == K_LEFT), next = k == K_DOWN || (horizontal && k == K_RIGHT);
    if (prev) *sel = (*sel - 1 + n) % n;
    else if (next) *sel = (*sel + 1) % n;
    return prev || next;
}

}  // namespace fmui
