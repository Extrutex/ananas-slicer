#!/usr/bin/env python3
"""End-to-end check of ananas-engine over its JSON-RPC protocol (one JSON object per line).

    ANANAS_ENGINE_BIN=<engine> ANANAS_PROFILES_DIR=<resources/profiles> python3 test_engine_protocol.py

Standard library only. Exits non-zero on the first failed expectation.
"""

import base64
import json
import math
import os
import struct
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


def write_box_stl(path, dx, dy, dz):
    """An ASCII STL of the box (0,0,0)-(dx,dy,dz). Triangles 0-1 are the x = 0 face, 2-3 the x = dx face."""
    v = lambda x, y, z: (x * dx, y * dy, z * dz)
    quads = [  # outward-facing quads: x=0, x=1, y=0, y=1, z=0, z=1
        [v(0, 0, 0), v(0, 0, 1), v(0, 1, 1), v(0, 1, 0)],
        [v(1, 0, 0), v(1, 1, 0), v(1, 1, 1), v(1, 0, 1)],
        [v(0, 0, 0), v(1, 0, 0), v(1, 0, 1), v(0, 0, 1)],
        [v(0, 1, 0), v(0, 1, 1), v(1, 1, 1), v(1, 1, 0)],
        [v(0, 0, 0), v(0, 1, 0), v(1, 1, 0), v(1, 0, 0)],
        [v(0, 0, 1), v(1, 0, 1), v(1, 1, 1), v(0, 1, 1)],
    ]
    triangles = []
    for a, b, c, d in quads:
        triangles += [(a, b, c), (a, c, d)]
    lines = ["solid box"]
    for t in triangles:
        lines.append("facet normal 0 0 0\n outer loop")
        lines += ["  vertex %.6f %.6f %.6f" % p for p in t]
        lines.append(" endloop\nendfacet")
    lines.append("endsolid box")
    Path(path).write_text("\n".join(lines) + "\n")


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
    expect({"analyze.stress", "analyze.chunk"} <= set(info["methods"]), "engine.info lists the stress analysis")

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

    # --- model ----------------------------------------------------------------------------------
    cube = str(DATA / "20mm_cube.obj")
    loaded = engine.result("model.load", {"path": cube})
    obj = loaded["objects"][0]
    expect(len(loaded["objects"]) == 1 and len(obj["parts"]) == 1, "model.load returns the object with one part")
    part = obj["parts"][0]
    vertices = struct.unpack("<%df" % (len(base64.b64decode(part["vertices"])) // 4), base64.b64decode(part["vertices"]))
    indices = struct.unpack("<%dI" % (len(base64.b64decode(part["indices"])) // 4), base64.b64decode(part["indices"]))
    expect(part["triangles"] == 12 and len(indices) == 36 and max(indices) < len(vertices) // 3,
           "the part's mesh decodes to 12 triangles with valid indices")
    size = [obj["bbox"]["max"][i] - obj["bbox"]["min"][i] for i in range(3)]
    expect(all(abs(v - 20) < 1e-3 for v in size), "the object's bounding box is 20 mm on every side")
    expect(obj["stats"]["open_edges"] == 0 and abs(obj["stats"]["volume"] - 8000) < 1 and obj["stats"]["shells"] == 1,
           "the mesh statistics report a closed 8000 mm^3 shell")
    response, _ = engine.call("model.load", {"path": "/no/such/model.stl"})
    expect(response["error"]["code"] == -32602, "model.load of a missing file answers -32602")

    def bbox_of(transform):
        box = engine.result("model.bounds", {"path": cube, "transforms": [transform]})["objects"][0]["bbox"]
        return box["min"], box["max"]

    low, high = bbox_of({"position": [100, 120]})
    expect(abs((low[0] + high[0]) / 2 - 100) < 1e-6 and abs((low[1] + high[1]) / 2 - 120) < 1e-6 and abs(low[2]) < 1e-6,
           "model.bounds centres the object at the position and sets it down on the bed")
    low, high = bbox_of({"position": [0, 0], "scale": [2, 1, 0.5]})
    expect(abs(high[0] - low[0] - 40) < 1e-3 and abs(high[2] - low[2] - 10) < 1e-3, "scale applies per axis")
    low, high = bbox_of({"position": [0, 0], "rotation": [0, 0, 45]})
    expect(abs(high[0] - low[0] - 20 * math.sqrt(2)) < 1e-3, "a rotation about Z widens the footprint to the diagonal")
    low, high = bbox_of({"position": [0, 0], "rotation": [45, 0, 0]})
    expect(abs(low[2]) < 1e-6 and abs(high[2] - 20 * math.sqrt(2)) < 1e-3, "a rotation about X is set down on the bed again")
    for bad in ({"scale": [0, 1, 1]}, {"rotation": [0, 0]}, {"position": [1]}, {"scale": [1, 1, 5000]}):
        response, _ = engine.call("model.bounds", {"path": cube, "transforms": [bad]})
        expect(response["error"]["code"] == -32602, f"model.bounds refuses {bad}")

    # --- slice with a placement, then the preview of its G-code ---------------------------------
    with tempfile.TemporaryDirectory() as out_dir:
        output = Path(out_dir) / "placed.gcode"
        placed = engine.result("slice", {**base, "model": cube, "output": str(output),
                                         "transforms": [{"position": [60, 70], "scale": [1, 1, 0.5]}]})
        expect(placed["layers"] == 50, "a model scaled to half height slices to half the layers")
        info_ = engine.result("preview.load", {"gcode": str(output)})
        expect(len(info_["layers"]) == 50 and info_["extrusion_segments"] > 1000, "preview.load describes the layers and segments")
        low, high = info_["bbox"]["min"], info_["bbox"]["max"]
        expect(40 < (low[0] + high[0]) / 2 < 80 and abs((low[1] + high[1]) / 2 - 70) < 15,
               "the toolpaths lie where the object was placed (skirt included)")
        expect(info_["speed"]["max"] > 0 and info_["flow"]["max"] > 0 and info_["accel"]["max"] > 0, "speed, flow and acceleration ranges are reported")
        expect({r["name"] for r in info_["roles"]} >= {"Outer wall", "Sparse infill"}, "the roles present are named")
        chunk = engine.result("preview.chunk", {"gcode": str(output), "layer_from": 0, "layer_to": 9})
        count = chunk["count"]
        positions = struct.unpack("<%df" % (count * 6), base64.b64decode(chunk["positions"]))
        speeds = struct.unpack("<%df" % count, base64.b64decode(chunk["speed"]))
        layers = struct.unpack("<%dH" % count, base64.b64decode(chunk["layer"]))
        expect(count > 100 and len(positions) == count * 6 and min(speeds) > 0 and max(layers) <= 9 and min(layers) == 0,
               f"preview.chunk returns typed arrays for the first ten layers ({count} segments)")
        z_by_layer = {layer: positions[i * 6 + 5] for i, layer in enumerate(layers)}
        expect(all(abs(z - info_["layers"][layer]["z"]) < 1e-3 for layer, z in z_by_layer.items()), "segment heights match the layer table")
        with_travel = engine.result("preview.chunk", {"gcode": str(output), "layer_from": 0, "layer_to": 9, "travel": True})
        expect(with_travel["count"] > count, "travel moves come with the travel flag only")
        response, _ = engine.call("preview.chunk", {"gcode": str(output), "layer_from": 5, "layer_to": 500})
        expect(response["error"]["code"] == -32602, "a layer range beyond the file answers -32602")
        response, _ = engine.call("preview.load", {"gcode": str(Path(out_dir) / "missing.gcode")})
        expect(response["error"]["code"] == -32602, "preview.load of a missing file answers -32602")

    # --- stress analysis ------------------------------------------------------------------------
    with tempfile.TemporaryDirectory() as stl_dir:
        stl = Path(stl_dir) / "beam.stl"
        write_box_stl(stl, 40.0, 4.0, 4.0)
        material = {"youngs_modulus": 2000, "poisson_ratio": 0.3, "yield_strength": 40}
        request = {
            "model": str(stl), "material": material, "voxel_size": 1.0,
            "transform": {"position": [20, 2]},  # the beam then spans x 0..40, y 0..4, z 0..4 (as model.bounds says)
            "supports": [{"box": {"min": [0, 0, 0], "max": [0, 4, 4]}}],
            "loads": [{"box": {"min": [40, 0, 0], "max": [40, 4, 4]}, "force": [0, 0, -10]}],
        }
        response, notifications = engine.call("analyze.stress", request)
        expect("result" in response, f"analyze.stress answers a result: {response.get('error')}")
        stress = response["result"]
        progress = [n["params"]["percent"] for n in notifications if n.get("method") == "analyze.progress"]
        expect(len(progress) >= 3 and progress == sorted(progress) and progress[-1] == 100
               and all(n["params"]["id"] == response["id"] for n in notifications),
               f"analyze.progress rises to 100 and carries the request id ({len(progress)} notifications)")
        expect(stress["grid"]["size"] == [40, 4, 4] and stress["solid_cells"] == 640, "the voxel grid is 40 x 4 x 4 cells")
        expect(stress["solver"]["converged"] and stress["solver"]["residual"] < 1e-5, "the solver converged")
        # Beam theory with shear (Timoshenko): 5.04 mm. Four cells over the height give about 4 % less.
        tip = stress["max_displacement"]["value"]
        expect(4.6 < tip < 5.05, f"the tip deflection is close to beam theory ({tip:.3f} mm against 5.04)")
        expect(abs(stress["reaction"][2] - 10) < 1e-3 and abs(stress["applied_force"][2] + 10) < 1e-9,
               "the support reaction balances the load")
        expect(stress["max_von_mises"]["value"] > 20 and stress["min_safety"]["value"] < 2
               and stress["max_von_mises"]["at"][0] < 2, "the highest stress sits at the clamp")
        chunk = engine.result("analyze.chunk", {"id": stress["id"], "z_from": 0, "z_to": 1})
        cells = chunk["count"]
        solid = base64.b64decode(chunk["solid"])
        vm = struct.unpack("<%df" % cells, base64.b64decode(chunk["von_mises"]))
        safety = struct.unpack("<%df" % cells, base64.b64decode(chunk["safety"]))
        vec = struct.unpack("<%df" % (cells * 3), base64.b64decode(chunk["displacement_vec"]))
        expect(cells == 2 * 40 * 4 and len(solid) == cells and sum(solid) == cells, "analyze.chunk returns two grid layers")
        expect(max(vm) == max(vm[:cells]) and max(vm) > 5 and min(safety) > 0 and min(vec[2::3]) < -1,
               "the chunk carries stress, safety and displacement per cell")
        response, _ = engine.call("analyze.chunk", {"id": stress["id"], "z_from": 2, "z_to": 9})
        expect(response["error"]["code"] == -32602, "a layer range beyond the grid answers -32602")
        response, _ = engine.call("analyze.chunk", {"id": stress["id"] + 1, "z_from": 0, "z_to": 0})
        expect(response["error"]["code"] == -32602, "analyze.chunk of an unknown analysis answers -32602")

        # Selection by triangle indices gives the same answer as the box.
        by_triangles = dict(request)
        by_triangles["supports"] = [{"triangles": [0, 1]}]  # the x = 0 face leads write_box_stl's triangle list
        by_triangles["loads"] = [{"triangles": [2, 3], "force": [0, 0, -10]}]
        same = engine.result("analyze.stress", by_triangles)
        expect(abs(same["max_displacement"]["value"] - tip) < 1e-4, "supports and loads can be picked by triangle index")

        # Z weakening: the safety falls, the in-plane stiffness stays.
        weak = engine.result("analyze.stress", {**request, "material": {**material, "z_factor": 0.5}})
        expect(weak["max_displacement"]["value"] > tip, "a weaker z direction deflects more")

        for bad, why in (({**request, "supports": []}, "no support"),
                         ({**request, "loads": []}, "no load"),
                         ({**request, "voxel_size": 0}, "a voxel size of zero"),
                         ({**request, "voxel_size": 0.001}, "a voxel size that makes billions of cells"),
                         ({**request, "material": {"youngs_modulus": 2000}}, "a material without yield strength"),
                         ({**request, "supports": [{"box": {"min": [90, 90, 90], "max": [99, 99, 99]}}]}, "a support in empty space"),
                         ({**request, "object": 5}, "a missing object"),
                         ({k: v for k, v in request.items() if k != "model"}, "no model"),
                         ({**request, "model": str(Path(stl_dir) / "none.stl")}, "a missing model file")):
            response, _ = engine.call("analyze.stress", bad)
            expect(response.get("error", {}).get("code") == -32602, f"analyze.stress refuses {why}")

        # A surface that is no solid never gets as far as the solver: the loader refuses it or the analysis does.
        broken = Path(stl_dir) / "surface.stl"
        broken.write_text("solid t\nfacet normal 0 0 1\n outer loop\n vertex 0 0 0\n vertex 10 0 0\n vertex 0 10 5\n"
                          " endloop\nendfacet\nendsolid t\n")
        response, _ = engine.call("analyze.stress", {**request, "model": str(broken)})
        expect(response.get("error", {}).get("code") == -32000, "a model without a closed volume answers -32000")

    engine.close()
    expect(all(json.loads(line) for line in engine.stdout_lines), "the protocol channel carried only JSON")
    print("PASS")


if __name__ == "__main__":
    main()
