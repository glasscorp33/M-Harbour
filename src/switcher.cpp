#include "switcher.h"

#include <stdio.h>
#include <string.h>

#include "library.h"

namespace switcher {

const char *outcome_name(Outcome o) {
    switch (o) {
        case CANCELLED: return "cancelled";
        case BATTERY_LOW: return "battery low";
        case AUTOSAVE_FAILED: return "auto-save failed";
        case REFUSED: return "refused";
        case READ_FAILED: return "read failed";
        case NOTHING_TO_DO: return "nothing to do";
        case WRITTEN: return "written";
        default: return "write failed";
    }
}

static bool empty(const writer::Plan &p) { return !p.to_write && !p.to_erase; }

Outcome load(const Request &req, const Hooks &h, Report *rep, char *err, size_t errlen) {
    memset(rep, 0, sizeof *rep);
    err[0] = 0;
    h.log("Load %s", req.name);
    if (h.battery && !h.battery(h.ctx, err, errlen)) {
        h.log("Not loaded: %s", err);
        return BATTERY_LOW;
    }

    // 1. the unit as it is now, kept before anything else
    if (!h.auto_save(h.ctx, err, errlen)) {
        h.log("Auto-save failed (%s): nothing written", err);
        return AUTOSAVE_FAILED;
    }

    // 2. the snapshot: this unit's, whole, intact, the same model
    if (!library::same_model(req.unit_model, req.snap_model)) {
        snprintf(rep->check.why, sizeof rep->check.why, "a %s snapshot, the unit is an %s", req.snap_model,
                 req.unit_model);
        rep->check.verdict = restore::OTHER_UNIT;
        snprintf(err, errlen, "%s", rep->check.why);
        h.log("Refused: %s", err);
        return REFUSED;
    }
    if (!restore::check(*req.info, req.smk37, req.size, req.notes, *req.src, h.progress, &rep->check, err, errlen)) {
        h.log("Reading the unit failed: %s", err);
        return READ_FAILED;
    }
    h.log("%s: %s", req.name, rep->check.why);
    if (!restore::allowed(rep->check.verdict)) {
        snprintf(err, errlen, "%s", rep->check.why);
        h.log("Refused: nothing written");
        return REFUSED;
    }

    // 3. what differs, the unit's boot area kept
    if (!jl::crc32(0, writer::BOOT_AREA, &rep->unit_boot_crc, err, errlen)) {
        h.log("Reading the unit failed: %s", err);
        return READ_FAILED;
    }
    rep->boot_differs = rep->unit_boot_crc != rep->check.boot_crc;
    h.log("Boot area: the unit's %08lX, the snapshot's %08lX%s", (unsigned long)rep->unit_boot_crc,
          (unsigned long)rep->check.boot_crc, rep->boot_differs ? "" : " (equal)");
    writer::Board board = req.smk37 ? writer::SMK37 : writer::FM1;
    writer::Image img = writer::full_image(board, req.name, req.src, writer::BOOT_AREA);
    rep->start = writer::BOOT_AREA;
    if (!writer::plan(img, &rep->plan, h.progress, err, errlen)) {
        h.log("Comparing failed: %s", err);
        return READ_FAILED;
    }
    h.log("From 0x4000: %lu sectors to write, %lu to erase", (unsigned long)rep->plan.to_write,
          (unsigned long)rep->plan.to_erase);
    if (empty(rep->plan) && !rep->boot_differs) {
        h.log("The unit already holds %s", req.name);
        return NOTHING_TO_DO;
    }

    // 4. confirmed by the user; the boot area only when asked, twice
    Choice c = h.confirm(h.ctx, 0, *rep);
    if (c == EXACT && rep->boot_differs) {
        img = writer::full_image(board, req.name, req.src, 0);
        rep->start = 0;
        if (!writer::plan(img, &rep->plan, h.progress, err, errlen)) {
            h.log("Comparing failed: %s", err);
            return READ_FAILED;
        }
        h.log("Exactly (boot area too): %lu sectors to write, %lu to erase", (unsigned long)rep->plan.to_write,
              (unsigned long)rep->plan.to_erase);
        c = h.confirm(h.ctx, 1, *rep) == WRITE ? WRITE : CANCEL;
    } else if (c == EXACT) {
        c = WRITE;  // the boot areas are equal: nothing more to write
    }
    if (c != WRITE) {
        h.log("Cancelled: nothing written");
        return CANCELLED;
    }
    if (empty(rep->plan)) {
        h.log("Nothing to write");
        return NOTHING_TO_DO;
    }
    if (h.battery && !h.battery(h.ctx, err, errlen)) {
        h.log("Not written: %s", err);
        return BATTERY_LOW;
    }

    // 5. the write
    h.log("Writing %s from 0x%05lX", req.name, (unsigned long)rep->start);
    if (!writer::apply(img, rep->plan, h.log, h.progress, err, errlen)) {
        h.log("Write stopped: %s", err);
        return WRITE_FAILED;
    }
    h.log("Loaded %s", req.name);
    return WRITTEN;
}

}  // namespace switcher
