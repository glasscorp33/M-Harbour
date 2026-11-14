// USB MIDI without any ESP-IDF: finding a device's MIDI OUT in its
// configuration descriptor, SysEx as USB-MIDI 1.0 event packets, and a string
// descriptor as text. Pure functions, so that they build and are tested on a PC
// (tests/host/test_usb_midi.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usbmidi {

// The soft key (M-Flasher's src/midi.rs): M-VAVE's firmwares (FM-1 015,
// SMK-37 Pro/Elite 016) and Felucca answer it on their first MIDI port by
// restarting into the ROM's USB download mode (4C4A:8057).
extern const uint8_t SOFT_KEY[6];

// The first MIDIStreaming interface (Audio class 0x01, subclass 0x03) with a
// bulk OUT endpoint.
struct MidiOut {
    uint8_t iface, alt;  // bInterfaceNumber, bAlternateSetting
    uint8_t ep;          // bEndpointAddress (OUT: bit 7 clear)
    uint16_t mps;        // wMaxPacketSize
};

enum Find {
    NO_MIDI,         // no MIDIStreaming interface at all (0: a zeroed result finds nothing)
    NO_BULK_OUT,     // MIDIStreaming, but no bulk OUT endpoint in any of them
    BAD_DESCRIPTOR,  // too short, or not a configuration descriptor
    FOUND,
};
const char *find_name(Find f);

// cfg: the whole configuration descriptor (wTotalLength bytes; len caps it).
Find find_midi_out(const uint8_t *cfg, size_t len, MidiOut *out);

// A SysEx message (F0 ... F7) as USB-MIDI event packets on `cable` (0-15):
// 4 bytes a packet, CIN 0x4 (start/continue, 3 bytes) then 0x5/0x6/0x7 (end
// with 1/2/3 bytes), unused bytes 0. Bytes written, or 0 when msg is not one
// SysEx message or out holds less than needed.
size_t sysex_packets(const uint8_t *msg, size_t len, uint8_t cable, uint8_t *out, size_t cap);

// A USB string descriptor (UTF-16LE) as ASCII ('?' for anything else), always
// terminated. Characters written.
size_t string_ascii(const uint8_t *desc, char *out, size_t n);

}  // namespace usbmidi
