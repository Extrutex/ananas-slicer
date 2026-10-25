#!/usr/bin/env python3
"""Dual-run comparison of the two precise outer wall methods on one binary.

Slices a corpus with the shipped Voron 2.4 350 presets once per precise_outer_wall_method
(outline_shrink, toolpath_shift), precise_outer_wall on, and reports per model:

  - extruded filament: net E over the whole file, change new vs legacy;
  - retractions (G10 or a negative E move after extruding; wipe moves that retract further do not count)
    and extrusion runs (extruding stretches between travels);
  - narrowest wall ;WIDTH: (outer and inner walls), against the narrowest legacy wall width;
  - outer wall extents, the part's outer dimensions as printed by the outer wall (criterion Q5);
  - slicing wall time (whole process, including preset loading) and peak memory (/usr/bin/time -l), median
    over --runs; the runs of the two methods alternate;
  - determinism: every run of one method gives the same file, ignoring the header timestamp;
  - spiral vase: both methods give the same file, ignoring the method's own config line.

Pass rules (wall analysis, old-vs-new protocol):
  - E: <= +3 % on the models listed in NON_THIN (no thin features);
  - retractions and runs: no increase > 5 %;
  - narrowest wall width: not below min_bead_width - 0.005 mm (criterion Q7);
  - outer wall extents (min/max X/Y of the outer wall bead edges over the file): within 0.005 mm, except on
    the thin-feature cases in THIN, where legacy drops features that toolpath_shift prints (reported only);
  - time and memory: median <= +5 % (reported per model; the time rule is meant for the large models);
  - determinism on every model and method (reported only with fuzzy skin, whose noise is random per run);
    spiral vase identical.

Hard cases are generated as STL. Embossed text and MMU-painted boundaries need project files and are not
part of this corpus.

usage: wall_method_comparison.py --bin <orca-slicer binary> --repo <source tree> --out <dir> [--runs 3]
                                 [--models name,name,...]
Exit status 0 when every row passes, 1 when a row fails, 2 on a usage or slicing error.
"""

import argparse
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time

METHODS = ("outline_shrink", "toolpath_shift")
NON_THIN = ("20mm_cube", "extruder_idler")
THIN = ("fins", "fins_0.6", "wedge", "pins", "transition", "terrace_one_wall_top", "overhang_rib")
# min_bead_width as a fraction of the nozzle: the Voron processes do not set it, so the 85 % default applies.
MIN_BEAD_WIDTH = 0.85
TOL_WIDTH = 0.005


# ---- Generated hard cases (mm). Every solid is a union of prisms over convex polygons. ----

def prism(poly, z0, z1, shear=(0.0, 0.0)):
    """Triangles of a convex counter-clockwise polygon extruded from z0 to z1; the top is moved by shear."""
    n = len(poly)
    top = [(p[0] + shear[0], p[1] + shear[1]) for p in poly]
    cx, cy = sum(p[0] for p in poly) / n, sum(p[1] for p in poly) / n
    tx, ty = cx + shear[0], cy + shear[1]
    out = []
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        ta, tb = top[i], top[(i + 1) % n]
        out.append(((cx, cy, z0), (b[0], b[1], z0), (a[0], a[1], z0)))
        out.append(((tx, ty, z1), (ta[0], ta[1], z1), (tb[0], tb[1], z1)))
        out.append(((a[0], a[1], z0), (b[0], b[1], z0), (tb[0], tb[1], z1)))
        out.append(((a[0], a[1], z0), (tb[0], tb[1], z1), (ta[0], ta[1], z1)))
    return out


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def circle(cx, cy, r, n=48):
    return [(cx + r * math.cos(2 * math.pi * k / n), cy + r * math.sin(2 * math.pi * k / n)) for k in range(n)]


def gen_fins():
    tris, x = [], 0.0
    for t in (0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2):
        tris += prism(rect(x, 0, x + t, 12), 0, 2)
        x += t + 3
    return tris


def gen_wedge():
    return prism([(0, -0.025), (40, -1.0), (40, 1.0), (0, 0.025)], 0, 2)


def gen_pins():
    tris, x = [], 0.0
    for d in (0.3, 0.4, 0.5, 0.6, 0.8, 1.0, 1.5, 2.0):
        tris += prism(circle(x + d / 2, 0, d / 2), 0, 3)
        x += d + 3
    return tris


def gen_holes():
    # One 7 mm square island per hole, as a closed mesh: the top and bottom are a ring of quads between the
    # hole's 48-gon and the square (48 is a multiple of 8, so the corners are ring points).
    tris, n, half, h = [], 48, 3.5, 2.0
    for i, d in enumerate((0.4, 0.6, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0)):
        cx, cy = 8.0 * i + 4, 4.0
        inner = circle(cx, cy, d / 2, n)
        outer = []
        for k in range(n):
            c, s = math.cos(2 * math.pi * k / n), math.sin(2 * math.pi * k / n)
            f = half / max(abs(c), abs(s))
            outer.append((cx + f * c, cy + f * s))
        for k in range(n):
            i0, i1, o0, o1 = inner[k], inner[(k + 1) % n], outer[k], outer[(k + 1) % n]
            tris.append(((o0[0], o0[1], h), (o1[0], o1[1], h), (i1[0], i1[1], h)))
            tris.append(((o0[0], o0[1], h), (i1[0], i1[1], h), (i0[0], i0[1], h)))
            tris.append(((o0[0], o0[1], 0), (i1[0], i1[1], 0), (o1[0], o1[1], 0)))
            tris.append(((o0[0], o0[1], 0), (i0[0], i0[1], 0), (i1[0], i1[1], 0)))
            tris.append(((o0[0], o0[1], 0), (o1[0], o1[1], 0), (o1[0], o1[1], h)))
            tris.append(((o0[0], o0[1], 0), (o1[0], o1[1], h), (o0[0], o0[1], h)))
            tris.append(((i1[0], i1[1], 0), (i0[0], i0[1], 0), (i0[0], i0[1], h)))
            tris.append(((i1[0], i1[1], 0), (i0[0], i0[1], h), (i1[0], i1[1], h)))
    return tris


def gen_transition():
    tris, x = [], 0.0
    for t in (0.60, 0.63, 0.66, 0.69, 0.72, 0.75):
        tris += prism(rect(x, 0, x + t, 15), 0, 2)
        x += t + 3
    return tris


def gen_corners():
    tris = []
    for i, deg in enumerate((15, 30)):
        x0, a = 30 * i, math.radians(deg)
        tris += prism([(x0, 0), (x0 + 20, 0), (x0 + 20 * math.cos(a), 20 * math.sin(a))], 0, 3)
    return tris


def gen_terrace():
    # A 1.2 mm rib standing on a 1 mm terrace: the layer under the terrace top has a top surface beside the rib.
    return prism(rect(0, 0, 1.2, 12), 0, 3) + prism(rect(-3, 3, 4.2, 9), 0, 1)


def gen_overhang_rib():
    # A 0.8 mm rib leaning 40 degrees: its outer wall is partly an overhang on every layer.
    h = 6.0
    return prism(rect(0, 0, 0.8, 15), 0, h, shear=(h * math.tan(math.radians(40)), 0))


GENERATED = {
    "fins": gen_fins,
    "wedge": gen_wedge,
    "pins": gen_pins,
    "holes": gen_holes,
    "transition": gen_transition,
    "corners": gen_corners,
    "terrace": gen_terrace,
    "overhang_rib": gen_overhang_rib,
}

# name -> (model source, process overrides, machine)
CORPUS = {
    "20mm_cube": ("tests/data/20mm_cube.obj", {}, "0.4"),
    "extruder_idler": ("tests/data/extruder_idler.obj", {}, "0.4"),
    "A": ("tests/data/A.obj", {}, "0.4"),
    "frog_legs": ("tests/data/frog_legs.obj", {}, "0.4"),
    "cube_with_hole": ("tests/data/cube_with_hole.obj", {}, "0.4"),
    "ringing_tower": ("resources/calib/input_shaping/ringing_tower.drc", {}, "0.4"),
    "fins": ("fins", {}, "0.4"),
    "fins_0.6": ("fins", {}, "0.6"),
    "wedge": ("wedge", {}, "0.4"),
    "pins": ("pins", {}, "0.4"),
    "holes": ("holes", {}, "0.4"),
    "transition": ("transition", {}, "0.4"),
    "corners": ("corners", {}, "0.4"),
    "terrace_one_wall_top": ("terrace", {"only_one_wall_top": "1"}, "0.4"),
    "overhang_rib": ("overhang_rib", {}, "0.4"),
    "cube_fuzzy_skin": ("tests/data/20mm_cube.obj", {"fuzzy_skin": "external"}, "0.4"),
    "cube_scarf_seam": ("tests/data/20mm_cube.obj", {"seam_slope_type": "external"}, "0.4"),
    "cube_spiral_vase": ("tests/data/20mm_cube.obj",
                         {"spiral_mode": "1", "wall_loops": "1", "top_shell_layers": "0", "sparse_infill_density": "0%"}, "0.4"),
}

MACHINES = {"0.4": ("Voron 2.4 350 0.4 nozzle", "0.20mm Standard @Voron"),
            "0.6": ("Voron 2.4 350 0.6 nozzle", "0.30mm Standard 0.6 nozzle @Voron")}


def write_stl(path, tris):
    with open(path, "w") as f:
        f.write("solid gen\n")
        for tri in tris:
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %.6f %.6f %.6f\n" % p)
            f.write("endloop\nendfacet\n")
        f.write("endsolid gen\n")


def flatten_process(profiles, name, overrides, out):
    folder = os.path.join(profiles, "Voron", "process")

    def load(n):
        with open(os.path.join(folder, n + ".json")) as f:
            d = json.load(f)
        if "inherits" in d:
            base = load(d.pop("inherits"))
            base.update(d)
            d = base
        return d

    d = load(name)
    d.update({"type": "process", "from": "User", "instantiation": "true", "name": os.path.basename(out)[:-5]})
    d.update(overrides)
    with open(out, "w") as f:
        json.dump(d, f)


# ---- G-code metrics ----

TIMESTAMP = re.compile(r"^; generated by .* on \d")


def analyse(gcode):
    e_total, retracts, runs, extruding, retracted = 0.0, 0, 0, False, False
    relative, e_pos = True, 0.0
    width, kind = None, ""
    min_wall = math.inf
    outer = [math.inf, math.inf, -math.inf, -math.inf]  # outer wall bead edge extents: x0, y0, x1, y1
    x = y = 0.0
    digest_body, digest_spiral = hashlib.sha256(), hashlib.sha256()
    with open(gcode, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace")
            if not TIMESTAMP.match(line):
                digest_body.update(raw)
                if not line.startswith("; precise_outer_wall_method ="):
                    digest_spiral.update(raw)
            if line.startswith(";TYPE:"):
                kind = line[6:].strip()
            elif line.startswith(";WIDTH:"):
                width = float(line[7:])
            elif line.startswith("M83"):
                relative = True
            elif line.startswith("M82"):
                relative = False
            elif line.startswith("G92"):
                m = re.search(r"\bE(-?[\d.]+)", line)
                if m:
                    e_pos = float(m.group(1))
            elif line.split(";")[0].strip() == "G10":
                retracts += 0 if retracted else 1
                retracted = True
            elif line.split(";")[0].strip() == "G11":
                retracted = False
            elif line.startswith(("G1 ", "G0 ", "G2 ", "G3 ")):
                code = line.split(";")[0]
                m = re.search(r"\bE(-?[\d.]+)", code)
                mx, my = re.search(r"\bX(-?[\d.]+)", code), re.search(r"\bY(-?[\d.]+)", code)
                moves = mx is not None or my is not None
                x0, y0 = x, y
                x, y = float(mx.group(1)) if mx else x, float(my.group(1)) if my else y
                if m:
                    e = float(m.group(1))
                    de = e if relative else e - e_pos
                    e_pos = e if not relative else e_pos
                    e_total += de
                    if de < 0:
                        retracts += 0 if retracted else 1
                        retracted = True
                    elif de > 0:
                        retracted = False
                    if de > 0 and moves:
                        if not extruding:
                            runs += 1
                        extruding = True
                        if kind in ("Outer wall", "Inner wall") and width is not None:
                            min_wall = min(min_wall, width)
                        if kind == "Outer wall" and width is not None:
                            h = width / 2
                            outer = [min(outer[0], x0 - h, x - h), min(outer[1], y0 - h, y - h),
                                     max(outer[2], x0 + h, x + h), max(outer[3], y0 + h, y + h)]
                    elif moves:
                        extruding = False
                elif moves:
                    extruding = False
    return {"e": e_total, "retracts": retracts, "runs": runs, "min_wall": min_wall, "outer": outer,
            "hash": digest_body.hexdigest(), "hash_spiral": digest_spiral.hexdigest()}


def slice_once(args, model, machine, process, outdir):
    os.makedirs(outdir, exist_ok=True)
    cmd = ["/usr/bin/time", "-l", args.bin, "--datadir", os.path.join(outdir, "datadir"),
           "--load-settings", "%s;%s" % (machine, process),
           "--load-filaments", os.path.join(args.repo, "resources/profiles/OrcaFilamentLibrary/filament/Generic PLA @System.json"),
           "--arrange", "1", "--slice", "0", "--outputdir", outdir, model]
    try:
        start = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
        elapsed = time.perf_counter() - start
    except subprocess.TimeoutExpired:
        raise RuntimeError("slicing timed out after %d s: %s" % (args.timeout, model))
    if p.returncode != 0:
        raise RuntimeError("slicer exited %d on %s:\n%s" % (p.returncode, model, p.stderr[-3000:]))
    gcodes = sorted(f for f in os.listdir(outdir) if f.endswith(".gcode"))
    if not gcodes:
        raise RuntimeError("no G-code for %s" % model)
    rss = re.search(r"(\d+)\s+maximum resident set size", p.stderr)
    peak = re.search(r"(\d+)\s+peak memory footprint", p.stderr)
    if rss is None and peak is None:
        raise RuntimeError("/usr/bin/time -l printed no memory for %s" % model)
    m = analyse(os.path.join(outdir, gcodes[0]))
    m["time"] = elapsed
    m["mem"] = int((peak or rss).group(1))
    return m


def pct(new, old):
    return 0.0 if old == 0 else 100.0 * (new - old) / old


def compare(name, res, nozzle, overrides):
    old, new = res["outline_shrink"], res["toolpath_shift"]
    med = lambda runs, k: statistics.median(r[k] for r in runs)
    rows, ok_all = [], True

    def row(metric, a, b, rule, ok):
        nonlocal ok_all
        ok_all &= ok
        rows.append("| %s | %s | %s | %s | %s |" % (metric, a, b, rule, "pass" if ok else "FAIL"))

    e_old, e_new = old[0]["e"], new[0]["e"]
    e_ok = name not in NON_THIN or pct(e_new, e_old) <= 3.0
    row("net E [mm]", "%.2f" % e_old, "%.2f (%+.2f %%)" % (e_new, pct(e_new, e_old)),
        "<= +3 %" if name in NON_THIN else "reported", e_ok)
    for k, label in (("retracts", "retractions"), ("runs", "extrusion runs")):
        a, b = old[0][k], new[0][k]
        row(label, str(a), "%d (%+.1f %%)" % (b, pct(b, a)), "<= +5 %", pct(b, a) <= 5.0)
    if math.isfinite(old[0]["outer"][0]) and math.isfinite(new[0]["outer"][0]):
        moved = max(abs(p - q) for p, q in zip(old[0]["outer"], new[0]["outer"]))
        row("outer wall extents moved [mm]", "", "%.4f" % moved, "reported" if name in THIN else "<= %.3f" % TOL_WIDTH,
            name in THIN or moved <= TOL_WIDTH)
    floor = MIN_BEAD_WIDTH * nozzle
    a, b = old[0]["min_wall"], new[0]["min_wall"]
    row("narrowest wall width [mm]", "%.4f" % a, "%.4f" % b, ">= min_bead_width %.3f - %.3f" % (floor, TOL_WIDTH),
        b >= floor - TOL_WIDTH)
    for k, label, unit in (("time", "slicing time", "s"), ("mem", "peak memory", "MB")):
        scale = 1.0 if k == "time" else 1.0 / (1024 * 1024)
        a, b = med(old, k) * scale, med(new, k) * scale
        row("%s (median of %d) [%s]" % (label, len(old), unit), "%.2f" % a, "%.2f (%+.1f %%)" % (b, pct(b, a)), "<= +5 %",
            pct(b, a) <= 5.0)
    random = overrides.get("fuzzy_skin", "none") != "none"
    for method in METHODS:
        hashes = {r["hash"] for r in res[method]}
        row("deterministic (%s)" % method, "", "%d distinct of %d runs" % (len(hashes), len(res[method])),
            "reported (fuzzy skin noise)" if random else "1 distinct", random or len(hashes) == 1)
    if name == "cube_spiral_vase":
        row("spiral vase identical", old[0]["hash_spiral"][:12], new[0]["hash_spiral"][:12], "equal",
            old[0]["hash_spiral"] == new[0]["hash_spiral"])
    table = ["### %s" % name, "", "| metric | outline_shrink | toolpath_shift | rule | result |", "|---|---|---|---|---|"] + rows
    return ok_all, "\n".join(table)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--models", default=",".join(CORPUS))
    args = ap.parse_args()
    if not os.access(args.bin, os.X_OK):
        print("binary not executable: %s" % args.bin, file=sys.stderr)
        return 2
    profiles = os.path.join(args.repo, "resources", "profiles")
    names = [n for n in args.models.split(",") if n]
    unknown = [n for n in names if n not in CORPUS]
    if unknown or args.runs < 1:
        print("unknown models: %s" % ", ".join(unknown) if unknown else "--runs must be >= 1", file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)

    reports, ok_all, raw = [], True, {}
    try:
        for name in names:
            source, overrides, nozzle = CORPUS[name]
            if source in GENERATED:
                model = os.path.join(args.out, source + ".stl")
                if not os.path.exists(model):
                    write_stl(model, GENERATED[source]())
            else:
                model = os.path.join(args.repo, source)
            machine_name, process_name = MACHINES[nozzle]
            machine = os.path.join(profiles, "Voron", "machine", machine_name + ".json")
            processes, res = {}, {method: [] for method in METHODS}
            for method in METHODS:
                processes[method] = os.path.join(args.out, "%s_%s.json" % (name, method))
                flatten_process(profiles, process_name, dict(overrides, precise_outer_wall="1", precise_outer_wall_method=method),
                                processes[method])
            # Runs alternate between the methods, so a change in machine load affects both alike.
            for i in range(args.runs):
                for method in METHODS:
                    res[method].append(slice_once(args, model, machine, processes[method], os.path.join(args.out, name, method, "run%d" % i)))
            print("sliced %s" % name, file=sys.stderr)
            raw[name] = res
            ok, table = compare(name, res, float(nozzle), overrides)
            ok_all &= ok
            reports.append(table)
    except RuntimeError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 2

    text = "\n\n".join(reports) + "\n\nOverall: %s\n" % ("pass" if ok_all else "FAIL")
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write(text)
    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump(raw, f, indent=1, default=lambda v: None if v == math.inf else v)
    print(text)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
