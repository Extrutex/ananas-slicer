#!/usr/bin/env python3
"""End-to-end check of ananas-engine over its JSON-RPC protocol (one JSON object per line).

    ANANAS_ENGINE_BIN=<engine> ANANAS_PROFILES_DIR=<resources/profiles> python3 test_engine_protocol.py

Standard library only. Exits non-zero on the first failed expectation.
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ENGINE = os.environ["ANANAS_ENGINE_BIN"]
PROFILES = os.environ["ANANAS_PROFILES_DIR"]
DATA = Path(os.environ.get("ANANAS_TEST_DATA_DIR", Path(__file__).resolve().parents[1] / "data"))
PRINTER_CFG = Path(__file__).resolve().parents[1] / "virtual-printer" / "printer.cfg"

PRINTER = "Voron 2.4 350 0.4 nozzle"
PROCESS = "0.20mm Standard @Voron"
FILAMENT = "Generic PLA @System"


class Session:
    def __init__(self):
        self.proc = subprocess.Popen(
            [ENGINE, "--profiles", PROFILES, "--vendor", "Voron"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.next_id = 1
        self.stdout_lines = []

    def send_raw(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read(self):
        line = self.proc.stdout.readline()
        if not line:
            raise AssertionError("engine closed the protocol channel")
        self.stdout_lines.append(line)
        return json.loads(line)  # every line on the channel must be JSON

    def call(self, method, params=None):
        """Returns (response, notifications received before it)."""
        request_id = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        self.send_raw(json.dumps(request))
        notifications = []
        while True:
            message = self.read()
            if message.get("id") == request_id:
                return message, notifications
            assert "id" not in message, f"unexpected response {message}"
            notifications.append(message)

    def result(self, method, params=None):
        response, _ = self.call(method, params)
        assert "result" in response, f"{method} failed: {response}"
        return response["result"]

    def close(self):
        self.proc.stdin.close()
        assert self.proc.wait(timeout=30) == 0, "engine did not exit cleanly on end of input"


def expect(condition, message):
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        sys.exit(1)
    print(f"ok   {message}")


def main():
    engine = Session()

    info = engine.result("engine.info")
    expect(info["name"] == "ananas-engine" and info["protocol"] == 1, "engine.info names the engine and protocol 1")
    expect("slice" in info["methods"], "engine.info lists slice")

    # --- errors ---------------------------------------------------------------------------------
    engine.send_raw("this is not json")
    error = engine.read()
    expect(error["error"]["code"] == -32700 and error["id"] is None, "invalid JSON answers -32700")
    response, _ = engine.call("no.such.method")
    expect(response["error"]["code"] == -32601, "unknown method answers -32601")
    response, _ = engine.call("config.resolve", {})
    expect(response["error"]["code"] == -32602, "missing printer answers -32602")
    response, _ = engine.call("config.resolve", {"printer": "No Such Printer"})
    expect(response["error"]["code"] == -32001, "unknown printer answers -32001")
    response, _ = engine.call("config.resolve", {"printer": PRINTER, "overrides": {"outer_wall_acceleration": "bad"}})
    expect(response["error"]["code"] == -32602, "an override that does not parse answers -32602")
    engine.send_raw(json.dumps({"jsonrpc": "2.0", "method": "engine.info"}))  # notification: no answer
    expect(engine.result("engine.info")["name"] == "ananas-engine", "a request without id gets no response")

    # --- presets --------------------------------------------------------------------------------
    printers = engine.result("presets.list")["printers"]
    expect(any(p["name"] == PRINTER for p in printers), f"presets.list contains {PRINTER}")
    compatible = engine.result("presets.list", {"printer": PRINTER})
    expect(PROCESS in compatible["processes"] and FILAMENT in compatible["filaments"],
           "presets.list for a printer lists its compatible process and filament presets")

    # --- schema ---------------------------------------------------------------------------------
    options = {o["key"]: o for o in engine.result("config.schema")["options"]}
    expect(len(options) > 500, f"config.schema lists the settings ({len(options)})")
    accel = options["outer_wall_acceleration"]
    expect(accel["scope"] == "print" and accel["unit"] == "mm/s²" and accel["label"], "schema carries scope, unit and label")
    expect(options["filament_max_volumetric_speed"]["scope"] == "filament", "schema tags the filament scope")
    expect(options["nozzle_diameter"]["scope"] == "printer" and options["nozzle_diameter"]["vector"], "schema tags printer and vector")
    expect(any("enum_values" in o for o in options.values()), "schema carries enum choices")

    # --- limits ---------------------------------------------------------------------------------
    imported = engine.result("limits.import", {"printer_cfg": PRINTER_CFG.read_text()})
    limits = imported["limits"]
    expect(imported["source"] == "printer_cfg" and limits["max_accel"] > 0 and limits["max_velocity"] > 0,
           "limits.import reads max_velocity and max_accel from printer.cfg")
    response, _ = engine.call("limits.import", {})
    expect(response["error"]["code"] == -32602, "limits.import without input answers -32602")

    base = {"printer": PRINTER, "process": PROCESS, "filament": FILAMENT}
    quiet = engine.result("limits.check", {**base, "limits": {**limits, "source": "printer_cfg"}})["violations"]
    over = engine.result("limits.check", {
        **base,
        "limits": {**limits, "max_accel": 3000, "source": "printer_cfg"},
        "overrides": {"outer_wall_acceleration": "20000"}})["violations"]
    hit = [v for v in over if v["key"] == "outer_wall_acceleration" and v["kind"] == "machine"]
    expect(len(hit) == 1 and hit[0]["state"] == "exceeded" and hit[0]["limit"] == 3000 and hit[0]["value"] == 20000
           and hit[0]["limit_key"] == "printer.max_accel" and hit[0]["source"] == "printer.cfg",
           "an acceleration above the imported max_accel is reported as exceeded with its source")
    caution = engine.result("limits.check", {
        **base,
        "limits": {**limits, "max_accel": 10000, "source": "printer_cfg"},
        "overrides": {"outer_wall_acceleration": "9500"}})["violations"]
    expect(any(v["key"] == "outer_wall_acceleration" and v["state"] == "caution" for v in caution),
           "95 % of the limit is a caution")
    expect(all(v["state"] in ("caution", "exceeded") for v in quiet), "limits.check answers with states for the imported limits")
    response, _ = engine.call("limits.check", {
        **base, "limits": {**limits, "pressure_advance_smooth_time": 0.5, "source": "moonraker"}})
    expect(response["error"]["code"] == -32602 and "smooth_time" in response["error"]["message"],
           "limits Klipper itself would refuse answer -32602")

    # The effective limits come back with every resolve, so a nominal row can show its limit too.
    defaults = engine.result("config.resolve", base)
    expect(defaults["limits"]["source"] == "profile" and defaults["limits"]["values"]["max_accel"] > 0,
           "config.resolve returns the effective limits and that they come from the profile")
    imported_view = engine.result("limits.check", {**base, "limits": {**limits, "source": "moonraker"}})
    expect(imported_view["limits"]["source"] == "moonraker" and imported_view["limits"]["values"]["max_accel"] == limits["max_accel"],
           "limits.check returns an imported snapshot as the effective limits with its source")

    resolved = engine.result("config.resolve", {**base, "overrides": {"layer_height": "0.28"}})
    expect(resolved["printer"] == PRINTER and resolved["process"] == PROCESS and resolved["config"]["layer_height"] == "0.28",
           "config.resolve returns the names and applies overrides")

    # --- slice ----------------------------------------------------------------------------------
    with tempfile.TemporaryDirectory() as out_dir:
        output = Path(out_dir) / "nested" / "cube.gcode"
        response, notifications = engine.call("slice", {**base, "model": str(DATA / "20mm_cube.obj"), "output": str(output)})
        expect("result" in response, f"slice succeeds: {response.get('error')}")
        result = response["result"]
        gcode = output.read_text()
        expect(Path(result["gcode"]) == output and "G1" in gcode, "slice writes the G-code where asked")
        expect(result["layers"] == 100 and result["time_s"] > 60 and result["filament_mm"] > 0,
               f"slice reports layers, time and filament ({result['layers']} layers, {result['time_s']:.0f} s)")
        percents = [n["params"]["percent"] for n in notifications if n["method"] == "slice.progress"]
        expect(percents and percents == sorted(percents) and percents[-1] >= 90, f"slice streams monotonic progress ({len(percents)} notifications)")
        expect(all(n["params"]["id"] == response["id"] for n in notifications), "progress notifications carry the request id")
        expect(isinstance(result["violations"], list) and result["limits"]["values"]["max_velocity"] > 0,
               "slice reports violations and the limits they were measured against")

        response, _ = engine.call("slice", {**base, "model": str(DATA / "no_such_model.stl"), "output": str(output)})
        expect(response["error"]["code"] == -32602, "slice with a missing model answers -32602")

    engine.close()
    expect(all(json.loads(line) for line in engine.stdout_lines), "the protocol channel carried only JSON")
    print("PASS")


if __name__ == "__main__":
    main()
