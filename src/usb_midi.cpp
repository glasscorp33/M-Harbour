#include "usb_midi.h"

namespace usbmidi {

const uint8_t SOFT_KEY[6] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};

const char *find_name(Find f) {
    switch (f) {
        case FOUND: return "found";
        case NO_MIDI: return "no MIDIStreaming interface";
        case NO_BULK_OUT: return "MIDIStreaming but no bulk OUT endpoint";
        default: return "bad configuration descriptor";
    }
}

Find find_midi_out(const uint8_t *cfg, size_t len, MidiOut *out) {
    if (!cfg || len < 9 || cfg[0] < 9 || cfg[1] != 0x02) return BAD_DESCRIPTOR;
    size_t total = cfg[2] | (cfg[3] << 8);
    if (total < len) len = total;
    bool midi_seen = false, in_ms = false;
    uint8_t iface = 0, alt = 0;
    for (size_t i = 0; i + 2 <= len; i += cfg[i]) {
        uint8_t bl = cfg[i], type = cfg[i + 1];
        if (bl < 2 || i + bl > len) break;  // malformed: stop where it goes wrong
        if (type == 0x04 && bl >= 9) {      // interface
            iface = cfg[i + 2];
            alt = cfg[i + 3];
            in_ms = cfg[i + 5] == 0x01 && cfg[i + 6] == 0x03;
            midi_seen |= in_ms;
        } else if (type == 0x05 && bl >= 7 && in_ms) {  // endpoint of a MIDIStreaming interface
            uint8_t ep = cfg[i + 2];
            if (!(ep & 0x80) && (cfg[i + 3] & 3) == 2) {
                out->iface = iface;
                out->alt = alt;
                out->ep = ep;
                out->mps = (cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FF;
                return FOUND;
            }
        }
    }
    return midi_seen ? NO_BULK_OUT : NO_MIDI;
}

size_t sysex_packets(const uint8_t *msg, size_t len, uint8_t cable, uint8_t *out, size_t cap) {
    if (!msg || len < 2 || msg[0] != 0xF0 || msg[len - 1] != 0xF7 || cable > 15) return 0;
    for (size_t i = 1; i + 1 < len; i++)
        if (msg[i] & 0x80) return 0;  // data bytes only between F0 and F7
    size_t need = (len + 2) / 3 * 4;
    if (cap < need) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        size_t n = len - i < 3 ? len - i : 3;
        bool last = i + n == len;
        uint8_t cin = last ? (uint8_t)(0x4 + n) : 0x4;  // 5, 6, 7: ends with 1, 2, 3 bytes
        out[o] = (uint8_t)(cable << 4 | cin);
        for (size_t k = 0; k < 3; k++) out[o + 1 + k] = k < n ? msg[i + k] : 0;
        o += 4;
    }
    return o;
}

size_t string_ascii(const uint8_t *desc, char *out, size_t n) {
    if (!n) return 0;
    size_t k = 0;
    if (desc && desc[0] >= 2 && desc[1] == 0x03) {
        for (size_t i = 2; i + 1 < desc[0] && k + 1 < n; i += 2) {
            uint16_t c = desc[i] | (desc[i + 1] << 8);
            out[k++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
        }
    }
    out[k] = 0;
    return k;
}

}  // namespace usbmidi
