#!/usr/bin/env python3
"""The engine's printer client against the virtual printer (real Klipper and Moonraker, no real printer).

    tests/engine/rig/rig.sh up
    ANANAS_RIG_URL=http://127.0.0.1:17125 ANANAS_ENGINE_BIN=<engine> ANANAS_PROFILES_DIR=<profiles> \
        python3 test_printer_rig.py

Without ANANAS_RIG_URL the test is skipped (exit 77). Standard library only.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

RIG = os.environ.get("ANANAS_RIG_URL")
if not RIG:
    print("skipped: ANANAS_RIG_URL is not set (start the rig with tests/engine/rig/rig.sh up)")
    sys.exit(77)  # CTest's SKIP_RETURN_CODE

ENGINE = os.environ["ANANAS_ENGINE_BIN"]
PROFILES = os.environ["ANANAS_PROFILES_DIR"]
DATA = Path(os.environ.get("ANANAS_TEST_DATA_DIR", Path(__file__).resolve().parents[1] / "data"))
BASE = {"printer": "Voron 2.4 350 0.4 nozzle", "process": "0.20mm Standard @Voron", "filament": "Generic PLA @System"}

proc = subprocess.Popen([ENGINE, "--profiles", PROFILES, "--vendor", "Voron"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, text=True)
next_id = 0


def call(method, params):
    global next_id
    next_id += 1
    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params}) + "\n")
    proc.stdin.flush()
    while True:
        message = json.loads(proc.stdout.readline())
        if message.get("id") == next_id:
            return message


def ok(method, params):
    answer = call(method, params)
    assert "result" in answer, f"{method} failed: {answer}"
    return answer["result"]


def expect(condition, message):
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    print(f"ok   {message}")


def http(path):
    with urllib.request.urlopen(RIG + path, timeout=30) as response:
        return response.read()


def wait_for_state(wanted, seconds=120):
    deadline = time.time() + seconds
    seen = []
    while time.time() < deadline:
        status = ok("moonraker.status", {"host": RIG})
        if not seen or seen[-1] != status["state"]:
            seen.append(status["state"])
        if status["state"] in wanted:
            return status, seen
        if status["state"] in ("error", "cancelled"):
            return status, seen
        time.sleep(0.5)
    return status, seen


# --- connecting --------------------------------------------------------------------------------
connected = ok("moonraker.connect", {"host": RIG})
expect(connected["klippy_state"] == "ready" and connected["klippy_connected"], "the printer is connected and Klipper is ready")
limits = connected["limits"]
expect(limits is not None and limits["max_velocity"] == 300 and limits["max_accel"] == 7000,
       "its Klipper limits come from the running Klipper (max_velocity 300, max_accel 7000)")
expect(ok("moonraker.connect", {"host": RIG.replace("http://", "")})["host"] == RIG, "a host without a scheme is read as http")

for params, code, why in [
    ({"host": "http://127.0.0.1:1"}, -32002, "an unreachable host answers -32002"),
    ({"host": "ftp://printer"}, -32602, "a scheme other than http(s) answers -32602"),
    ({"host": "http://user:secret@printer"}, -32602, "credentials in the URL answer -32602"),
    ({}, -32602, "a missing host answers -32602"),
]:
    expect(call("moonraker.connect", params)["error"]["code"] == code, why)
answer = call("moonraker.upload", {"host": RIG, "gcode": "/no/such/file.gcode"})
expect(answer["error"]["code"] == -32602, "uploading a file that does not exist answers -32602")
for name in ("../escape.gcode", "a/b.gcode", ".."):
    answer = call("moonraker.upload", {"host": RIG, "gcode": str(DATA / "20mm_cube.obj"), "name": name})
    expect(answer["error"]["code"] == -32602, f"the name {name!r} is refused")

# --- slice with the printer's own limits, upload, print ------------------------------------------
imported = {**limits, "source": "moonraker"}
with tempfile.TemporaryDirectory() as out:
    gcode = Path(out) / "cube from the engine.gcode"
    sliced = ok("slice", {**BASE, "model": str(DATA / "20mm_cube.obj"), "output": str(gcode), "limits": imported})
    expect(sliced["limits"]["source"] == "moonraker", "the slice was measured against the printer's limits")

    uploaded = ok("moonraker.upload", {"host": RIG, "gcode": str(gcode)})
    expect(uploaded["name"] == gcode.name and uploaded["print_started"] is False, "the G-code is uploaded under its file name without starting")
    listing = json.loads(http("/server/files/list?root=gcodes"))["result"]
    expect(any(item["path"] == gcode.name for item in listing), "the printer lists the file (a name with spaces)")
    stored = http("/server/files/gcodes/" + urllib.parse.quote(gcode.name))
    expect(hashlib.sha256(stored).hexdigest() == hashlib.sha256(gcode.read_bytes()).hexdigest(), "the stored file is byte for byte the slice")
    idle = ok("moonraker.status", {"host": RIG})
    expect(idle["state"] in ("standby", "complete"), f"nothing prints yet ({idle['state']})")

    ok("moonraker.start", {"host": RIG, "name": gcode.name})
    status, seen = wait_for_state({"complete"})
    expect(status["state"] == "complete" and status["filename"] == gcode.name, f"the print runs to the end ({' > '.join(seen)})")
    expect(status["progress"] >= 0.99, f"progress reaches 100 % ({status['progress']:.2f})")

    # Upload and print in one request.
    second = Path(out) / "printed at once.gcode"
    second.write_bytes(gcode.read_bytes())
    again = ok("moonraker.upload", {"host": RIG, "gcode": str(second), "print": True})
    expect(again["print_started"] is True, "uploading with print starts the print")
    status, seen = wait_for_state({"complete"})
    expect(status["state"] == "complete" and status["filename"] == second.name, f"that print runs to the end ({' > '.join(seen)})")

    # A start for a file the printer does not have is refused, with the printer's own message.
    missing = call("moonraker.start", {"host": RIG, "name": "not there.gcode"})
    expect(missing["error"]["code"] == -32002, "starting a file that is not there is refused as a printer error")

proc.stdin.close()
proc.wait(timeout=30)
print("PASS")
