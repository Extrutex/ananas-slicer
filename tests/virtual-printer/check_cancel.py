#!/usr/bin/env python3
"""Cancel a running print through the rig's Moonraker and check what is left.

Runs inside the moonraker container against http://127.0.0.1:7125, with Klippy
on state-macros/printer-cancel.cfg (Mainsail's CANCEL_PRINT and its
user_cancel_macro hook calling _ANANAS_SLICER_RESTORE). Steps:

1. CANCEL_PRINT must be the client's macro (_VP_EXPECT_CLIENT_CANCEL).
2. Upload state-macros/cancel.gcode and start it. It saves the motion limits,
   sets per-feature values, switches the heaters and the part cooling fan on
   and then loops until it is cancelled.
3. Once print_stats says "printing" and the loop has run, check that the
   print's values are in place, then POST /printer/print/cancel, as a client's
   cancel button does.
4. print_stats must end "cancelled", virtual_sdcard must be idle, the heaters
   and the part cooling fan off, the motion limits and printer.cfg pressure
   advance back, the snapshot gone, and no G-code after the cancel point may
   have run: the toolhead's print time, which each pass of the loop
   advances, stays put, and the line after the loop never runs.

Writes every API response used to a JSON file. Exit status: 0 pass, 1 fail,
2 usage.
"""

import argparse
import json
import os
import sys
import time

from check_moonraker import ApiError, request, upload, wait_for

LIMITS = {"max_velocity": 300, "max_accel": 7000, "minimum_cruise_ratio": 0.5,
          "square_corner_velocity": 5}
PRINT_LIMITS = {"max_velocity": 120, "max_accel": 1500, "minimum_cruise_ratio": 0.2,
                "square_corner_velocity": 9}
# Object name -> printer.cfg (pressure_advance, smooth_time), and the values
# cancel.gcode sets.
PRESSURE_ADVANCE = {"extruder": (0.04, 0.04), "extruder_stepper Belt": (0.06, 0.03)}
PRINT_PRESSURE_ADVANCE = {"extruder": (0.2, 0.02), "extruder_stepper Belt": (0.3, 0.01)}
HEATERS = ("extruder", "extruder1", "heater_bed")
STATE = "gcode_macro _VP_CANCEL_STATE"
SAVE = "gcode_macro _ANANAS_SLICER_SAVE"
# Seconds the print time must stand still after the cancel.
SETTLE_S = 2.0


def script(gcode):
    return request("POST", "/printer/gcode/script", {"script": gcode})


def query():
    objects = {name: "" for name in HEATERS + tuple(PRESSURE_ADVANCE)}
    objects.update({"print_stats": "", "virtual_sdcard": "", "fan": "", "toolhead": "",
                    STATE: "", SAVE: ""})
    return request("GET", "/printer/objects/query", objects)["result"]["status"]


def close(a, b):
    return abs(float(a) - float(b)) < 1e-9


def check_values(status, limits, pressure_advance, when, fail):
    th = status["toolhead"]
    for key, want in limits.items():
        if not close(th[key], want):
            fail("%s: toolhead %s %s, expected %s" % (when, key, th[key], want))
    for name, (pa, st) in pressure_advance.items():
        obj = status[name]
        if not close(obj["pressure_advance"], pa) or not close(obj["smooth_time"], st):
            fail("%s: %s pressure advance %s / %s, expected %s / %s" % (
                when, name, obj["pressure_advance"], obj["smooth_time"], pa, st))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcode", required=True, help="G-code file to upload and cancel")
    parser.add_argument("--remote-name", required=True, help="file name on the printer")
    parser.add_argument("--result", required=True, help="JSON file for the API responses")
    args = parser.parse_args()
    if not os.path.isfile(args.gcode):
        print("check_cancel: not a file: %s" % args.gcode, file=sys.stderr)
        return 2

    result = {"passed": False, "failures": [], "responses": {}}
    failures = result["failures"]
    responses = result["responses"]
    fail = failures.append

    try:
        info = request("GET", "/server/info")["result"]
        responses["server_info"] = info
        if info.get("klippy_state") != "ready":
            raise ApiError("Klippy state is %r, not 'ready'" % info.get("klippy_state"))
        # An HTTP error here means the rig's CANCEL_PRINT would be tested instead.
        script("_VP_EXPECT_CLIENT_CANCEL")

        responses["upload"] = upload(args.gcode, args.remote_name)
        store_before = len(request("GET", "/server/gcode_store", {"count": 1000})
                           ["result"]["gcode_store"])
        responses["print_start"] = request(
            "POST", "/printer/print/start", {"filename": args.remote_name})

        def poll_looping():
            status = query()
            state = status["print_stats"]["state"]
            if state in ("complete", "error", "cancelled") or (state == "standby" and status[STATE]["reached"]):
                raise ApiError("print ended in state %r before the cancel: %s" % (
                    state, status["print_stats"].get("message", "")))
            if state == "printing" and status[STATE]["reached"]:
                return status
            return None

        reached = wait_for("the print to reach its loop", 60, poll_looping)
        time.sleep(0.5)
        before = query()
        responses["before_cancel"] = before
        if before["print_stats"]["state"] != "printing":
            fail("before the cancel: print_stats %r, not 'printing'" % before["print_stats"]["state"])
        if not before["toolhead"]["print_time"] > reached["toolhead"]["print_time"]:
            fail("before the cancel: the print's loop does not run (print time stays at %s)"
                 % before["toolhead"]["print_time"])
        if not before["virtual_sdcard"]["is_active"]:
            fail("before the cancel: virtual_sdcard is not active")
        for name in HEATERS:
            if before[name]["target"] == 0:
                fail("before the cancel: %s target 0, the print did not heat" % name)
        check_values(before, PRINT_LIMITS, PRINT_PRESSURE_ADVANCE, "before the cancel", fail)
        if not before[SAVE]["saved"]:
            fail("before the cancel: _ANANAS_SLICER_SAVE recorded no snapshot")

        # What a client's cancel button sends: Moonraker runs CANCEL_PRINT.
        responses["print_cancel"] = request("POST", "/printer/print/cancel")

        def poll_cancelled():
            status = query()
            return status if status["print_stats"]["state"] != "printing" else None

        after = wait_for("the print to stop", 30, poll_cancelled)
        responses["after_cancel"] = after
        state = after["print_stats"]["state"]
        if state != "cancelled":
            fail("print_stats ended in state %r, expected 'cancelled': %s" % (
                state, after["print_stats"].get("message", "")))

        # The fan reports a new speed only once the MCU queue reaches it:
        # _VP_EXPECT_FAN_OFF dwells past that and raises a G-code error,
        # an HTTP error here, if the fan is still on.
        time.sleep(SETTLE_S)
        try:
            script("_VP_EXPECT_FAN_OFF")
        except ApiError as exc:
            fail("part cooling fan after the cancel: %s" % exc)
        settled = query()
        responses["settled"] = settled
        if settled["print_stats"]["state"] != state:
            fail("print_stats changed from %r to %r after the cancel" % (
                state, settled["print_stats"]["state"]))
        sd = settled["virtual_sdcard"]
        if sd["is_active"] or sd["file_path"] is not None:
            fail("virtual_sdcard still active or holding %s after the cancel" % sd["file_path"])
        for name in HEATERS:
            if settled[name]["target"] != 0:
                fail("%s target %s after the cancel" % (name, settled[name]["target"]))
        if settled["fan"]["speed"] != 0:
            fail("part cooling fan speed %s after the cancel" % settled["fan"]["speed"])
        check_values(settled, LIMITS, PRESSURE_ADVANCE, "after the cancel", fail)
        if settled[SAVE]["saved"]:
            fail("after the cancel: the _ANANAS_SLICER_SAVE snapshot is still there, the restore did not run")
        time.sleep(SETTLE_S)
        final = query()
        responses["final"] = final
        if final["toolhead"]["print_time"] != settled["toolhead"]["print_time"]:
            fail("G-code ran on after the cancel (print time %s -> %s)" % (
                settled["toolhead"]["print_time"], final["toolhead"]["print_time"]))
        if final[STATE]["after"]:
            fail("G-code after the cancel point ran")

        store = request("GET", "/server/gcode_store", {"count": 1000})["result"]["gcode_store"]
        responses["gcode_store_during_check"] = store[store_before:] if store_before <= len(store) else store
        for entry in responses["gcode_store_during_check"]:
            msg = entry.get("message", "")
            if msg.startswith("!!") or "Unknown command" in msg:
                fail("G-code response during the check: %s" % msg)

        hist = request("GET", "/server/history/list", {"limit": 1, "order": "desc"})["result"]
        responses["history_last"] = hist
        jobs = hist.get("jobs") or []
        if not jobs or jobs[0].get("status") != "cancelled":
            fail("history: last job %s, expected status 'cancelled'" % (
                jobs[0].get("status") if jobs else "missing"))
    except (ApiError, KeyError, TypeError, ValueError) as exc:
        fail("%s: %s" % (type(exc).__name__, exc))

    result["passed"] = not failures
    with open(args.result, "w") as f:
        json.dump(result, f, indent=2)
    for msg in failures:
        print("FAIL: " + msg, file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
