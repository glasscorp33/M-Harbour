// USB host side of the JieLi ROM download mode: the ESP32-S3's USB host on the
// Cardputer's USB-C port, and the mass storage Bulk-Only Transport the ROM
// (WL82 UBOOT1.00, 4C4A:8057) and M-Flasher's loader speak.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace host {

enum Result { OK = 0, REFUSED = 1, FAILED = 2 };

// Installs the USB host on the internal PHY (takes GPIO19/20 from the app).
bool start();
typedef void (*Log)(const char *fmt, ...);

// Waits for a device to enumerate; fills vid/pid. Claims its bulk interface.
// Logs the root port's state as it changes, and ESP-IDF's own USB messages.
// False with vid set: a device came, but it has no bulk interface (it stays
// open: device() describes it). tick (nullptr: none) is called every 20 ms
// or so while nothing is there; true cancels (false, vid 0).
bool wait_device(uint32_t timeout_ms, uint16_t *vid, uint16_t *pid, Log log, char *err, size_t errlen,
                 bool (*tick)() = nullptr);
bool connected();

// The device wait_device() opened: its IDs, product string ("" without one)
// and active configuration descriptor (nullptr without one).
struct Device {
    uint16_t vid, pid;
    char product[48];
    const uint8_t *config;
    size_t config_len;
};
bool device(Device *d);

// One bulk OUT transfer to ep of interface iface (claimed first, at alt), for
// a device that is NOT the download mode (the soft key over USB MIDI).
// sent: bytes the device took; status: the transfer's usb_transfer_status_t,
// -1 when it was not submitted. True when it completed.
bool bulk_out(uint8_t iface, uint8_t alt, uint8_t ep, const uint8_t *data, size_t len, uint32_t *sent, int *status,
              char *err, size_t errlen);
const char *status_name(int status);

// Waits until the open device leaves the bus: true. False after timeout_ms,
// or when tick (called every 20 ms or so) returns true.
bool wait_gone(uint32_t timeout_ms, bool (*tick)());
// Releases the device's interface and closes it, so that ESP-IDF frees it and
// the next device can be opened with wait_device().
void close_device(Log log);

// One Bulk-Only Transport command. in: data from the device into buf.
// moved: data bytes actually transferred. err: text for a non-OK result.
Result execute(const uint8_t *cdb, uint8_t cdb_len, uint8_t *buf, uint32_t len, bool in,
               uint32_t *moved, char *err, size_t errlen, bool quiet = false);

}  // namespace host
