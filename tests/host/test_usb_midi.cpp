// Host tests for src/usb_midi.cpp: the configuration descriptor parser, the
// SysEx packetising (the soft key) and string descriptors. tests/host/run.sh.
#include <stdio.h>
#include <string.h>

#include "usb_midi.h"

static int failures, checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            failures++;                                                  \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                \
    } while (0)

// A class-compliant USB-MIDI 1.0 device as the spec's example (USB Device
// Class Definition for MIDI Devices 1.0, appendix B) and most devices have it:
// Audio Control interface 0, MIDIStreaming interface 1 with one embedded and
// one external jack each way, bulk OUT 0x01 and bulk IN 0x81, 64 bytes.
static const uint8_t MIDI_CFG[] = {
    // configuration: 101 bytes, 2 interfaces
    0x09, 0x02, 0x65, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    // interface 0: Audio Control
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    // class-specific AC header: 1 streaming interface (1)
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01,
    // interface 1: MIDIStreaming, 2 endpoints
    0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    // MS header
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
    // MIDI IN jacks: embedded 1, external 2
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
    // MIDI OUT jacks: embedded 3 (from 2), external 4 (from 1)
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
    // bulk OUT 0x01, 64 bytes (audio-class endpoint: 9 bytes)
    0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    // CS endpoint: 1 embedded jack (1)
    0x05, 0x25, 0x01, 0x01, 0x01,
    // bulk IN 0x81, 64 bytes
    0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    // CS endpoint: 1 embedded jack (3)
    0x05, 0x25, 0x01, 0x01, 0x03,
};

// A USB mass storage device (the ROM's download mode looks like this): one
// interface, class 08, bulk IN 0x81 and OUT 0x02.
static const uint8_t MSC_CFG[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
};

static void test_midi_descriptor() {
    puts("usb_midi: a class-compliant USB-MIDI 1.0 descriptor");
    CHECK(sizeof MIDI_CFG == 0x65);
    usbmidi::MidiOut m = {};
    CHECK(usbmidi::find_midi_out(MIDI_CFG, sizeof MIDI_CFG, &m) == usbmidi::FOUND);
    CHECK(m.iface == 1 && m.alt == 0 && m.ep == 0x01 && m.mps == 64);
}

static void test_composite() {
    puts("usb_midi: MIDI after other interfaces (composite device)");
    // an HID interface 0 with an interrupt OUT, then the same MIDI as 1/2
    uint8_t cfg[256];
    const uint8_t hid[] = {0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
                           0x07, 0x05, 0x03, 0x03, 0x08, 0x00, 0x0A};
    size_t n = 0;
    memcpy(cfg, MIDI_CFG, 9);
    n = 9;
    memcpy(cfg + n, hid, sizeof hid);
    n += sizeof hid;
    memcpy(cfg + n, MIDI_CFG + 9, sizeof MIDI_CFG - 9);
    n += sizeof MIDI_CFG - 9;
    // interfaces renumbered: AC 1, MS 2, and the MS on endpoint 0x05
    cfg[9 + sizeof hid + 2] = 1;
    cfg[9 + sizeof hid + 18 + 2] = 2;
    size_t ep = 9 + sizeof hid + (9 + 9 + 9 + 7 + 6 + 6 + 9 + 9);
    CHECK(cfg[ep + 1] == 0x05 && cfg[ep + 2] == 0x01);
    cfg[ep + 2] = 0x05;
    cfg[2] = (uint8_t)n;
    cfg[3] = (uint8_t)(n >> 8);
    usbmidi::MidiOut m = {};
    CHECK(usbmidi::find_midi_out(cfg, n, &m) == usbmidi::FOUND);
    CHECK(m.iface == 2 && m.ep == 0x05 && m.mps == 64);
}

static void test_no_midi() {
    puts("usb_midi: no MIDI interface (mass storage)");
    usbmidi::MidiOut m = {};
    CHECK(usbmidi::find_midi_out(MSC_CFG, sizeof MSC_CFG, &m) == usbmidi::NO_MIDI);
}

static void test_no_bulk_out() {
    puts("usb_midi: MIDIStreaming with only an IN endpoint");
    uint8_t cfg[sizeof MIDI_CFG];
    memcpy(cfg, MIDI_CFG, sizeof cfg);
    size_t ep = 9 + 9 + 9 + 9 + 7 + 6 + 6 + 9 + 9;
    CHECK(cfg[ep + 2] == 0x01);
    cfg[ep + 2] = 0x82;  // the OUT made an IN
    usbmidi::MidiOut m = {};
    CHECK(usbmidi::find_midi_out(cfg, sizeof cfg, &m) == usbmidi::NO_BULK_OUT);
    // an interrupt OUT is not a bulk OUT either
    memcpy(cfg, MIDI_CFG, sizeof cfg);
    cfg[ep + 3] = 0x03;
    CHECK(usbmidi::find_midi_out(cfg, sizeof cfg, &m) == usbmidi::NO_BULK_OUT);
}

static void test_bad() {
    puts("usb_midi: truncated and malformed descriptors");
    usbmidi::MidiOut m = {};
    CHECK(usbmidi::find_midi_out(nullptr, 0, &m) == usbmidi::BAD_DESCRIPTOR);
    CHECK(usbmidi::find_midi_out(MIDI_CFG, 5, &m) == usbmidi::BAD_DESCRIPTOR);
    CHECK(usbmidi::find_midi_out(MSC_CFG + 9, sizeof MSC_CFG - 9, &m) == usbmidi::BAD_DESCRIPTOR);
    // cut before the OUT endpoint: the MIDI interface is seen, its endpoint not
    CHECK(usbmidi::find_midi_out(MIDI_CFG, 9 + 9 + 9 + 9 + 7 + 6 + 6 + 9 + 9 + 4, &m) == usbmidi::NO_BULK_OUT);
    // a zero length must not loop
    uint8_t cfg[sizeof MIDI_CFG];
    memcpy(cfg, MIDI_CFG, sizeof cfg);
    cfg[18] = 0;
    CHECK(usbmidi::find_midi_out(cfg, sizeof cfg, &m) == usbmidi::NO_MIDI);
    // wTotalLength shorter than the buffer: only wTotalLength counts
    memcpy(cfg, MIDI_CFG, sizeof cfg);
    cfg[2] = 9 + 9 + 9;
    CHECK(usbmidi::find_midi_out(cfg, sizeof cfg, &m) == usbmidi::NO_MIDI);
}

static void test_soft_key_packets() {
    puts("usb_midi: the soft key as USB-MIDI packets");
    uint8_t out[16];
    memset(out, 0xAA, sizeof out);
    size_t n = usbmidi::sysex_packets(usbmidi::SOFT_KEY, sizeof usbmidi::SOFT_KEY, 0, out, sizeof out);
    const uint8_t want[] = {0x04, 0xF0, 0x22, 0x24, 0x07, 0x35, 0x7D, 0xF7};
    CHECK(n == 8);
    CHECK(!memcmp(out, want, 8));
    CHECK(out[8] == 0xAA);
    // cable 1 in the high nibble
    n = usbmidi::sysex_packets(usbmidi::SOFT_KEY, 6, 1, out, sizeof out);
    CHECK(n == 8 && out[0] == 0x14 && out[4] == 0x17);
    // too small a buffer
    CHECK(usbmidi::sysex_packets(usbmidi::SOFT_KEY, 6, 0, out, 7) == 0);
}

static void test_sysex_ends() {
    puts("usb_midi: SysEx ending with 1, 2 or 3 bytes; not SysEx");
    uint8_t out[16];
    const uint8_t two[] = {0xF0, 0xF7};
    CHECK(usbmidi::sysex_packets(two, 2, 0, out, sizeof out) == 4);
    CHECK(out[0] == 0x06 && out[1] == 0xF0 && out[2] == 0xF7 && out[3] == 0);
    const uint8_t four[] = {0xF0, 0x01, 0x02, 0xF7};
    CHECK(usbmidi::sysex_packets(four, 4, 0, out, sizeof out) == 8);
    CHECK(out[0] == 0x04 && out[4] == 0x05 && out[5] == 0xF7 && out[6] == 0 && out[7] == 0);
    const uint8_t five[] = {0xF0, 0x01, 0x02, 0x03, 0xF7};
    CHECK(usbmidi::sysex_packets(five, 5, 0, out, sizeof out) == 8);
    CHECK(out[4] == 0x06 && out[5] == 0x03 && out[6] == 0xF7 && out[7] == 0);
    const uint8_t note[] = {0x90, 0x40, 0x7F};
    CHECK(usbmidi::sysex_packets(note, 3, 0, out, sizeof out) == 0);
    const uint8_t open[] = {0xF0, 0x01, 0x02};
    CHECK(usbmidi::sysex_packets(open, 3, 0, out, sizeof out) == 0);
    const uint8_t status_inside[] = {0xF0, 0x90, 0xF7};
    CHECK(usbmidi::sysex_packets(status_inside, 3, 0, out, sizeof out) == 0);
    CHECK(usbmidi::sysex_packets(usbmidi::SOFT_KEY, 6, 16, out, sizeof out) == 0);
}

static void test_strings() {
    puts("usb_midi: string descriptors");
    const uint8_t s[] = {0x0E, 0x03, 'S', 0, 'M', 0, 'K', 0, '-', 0, '3', 0, 0xE9, 0x00};
    char out[16];
    CHECK(usbmidi::string_ascii(s, out, sizeof out) == 6);
    CHECK(!strcmp(out, "SMK-3?"));
    CHECK(usbmidi::string_ascii(s, out, 4) == 3 && !strcmp(out, "SMK"));
    CHECK(usbmidi::string_ascii(nullptr, out, sizeof out) == 0 && !out[0]);
    const uint8_t odd[] = {0x05, 0x03, 'A', 0, 'B'};  // odd length: the last half is ignored
    CHECK(usbmidi::string_ascii(odd, out, sizeof out) == 1 && !strcmp(out, "A"));
}

int main() {
    test_midi_descriptor();
    test_composite();
    test_no_midi();
    test_no_bulk_out();
    test_bad();
    test_soft_key_packets();
    test_sysex_ends();
    test_strings();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
