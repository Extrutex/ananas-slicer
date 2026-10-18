#!/usr/bin/env python3
"""Build the Klipper timing corpus and its reference timings.

The corpus is G-code the KLIPSLICE time estimate is checked against: real
slices, reduced to what Klipper executes (comments and blank lines removed,
optionally cut to the first layers), so every line of a corpus file is one
command and line numbers mean the same to Klipper, to this script and to the
GCodeProcessor test that reads the reference.

  normalize  RAW OUT --start-position X,Y,Z [--max-layers N] [--legacy-limits]
  reference  --corpus DIR --results DIR --markers CSV --klipper-version V --out FILE
  compare    A.json B.json --tolerance SECONDS

"reference" pairs the k-th marker command of each corpus file with the k-th
marker time Klipper batch mode recorded for it (klippy_batch.py --markers).
Exit status: 0 success, 1 failure, 2 usage.
"""

import argparse
import json
import os
import re
import sys

LAYER_RE = re.compile(r"^SET_PRINT_STATS_INFO\b.*\bCURRENT_LAYER=(\d+)", re.IGNORECASE)
ACCEL_RE = re.compile(r"^SET_VELOCITY_LIMIT\s+ACCEL=([0-9.]+)\s*$", re.IGNORECASE)


def fail(msg):
    print("timing_corpus: %s" % msg, file=sys.stderr)
    return 1


def command_of(line):
    return line.split(None, 1)[0].upper() if line.strip() else ""


def strip_line(line):
    # Klipper drops everything from the first ';' (gcode.py _process_commands).
    return line.split(";", 1)[0].strip()


def cmd_normalize(args):
    with open(args.raw, encoding="utf-8") as f:
        lines = [strip_line(line) for line in f]
    lines = [line for line in lines if line]
    start = next((i for i, line in enumerate(lines) if command_of(line) == "PRINT_START"), None)
    if start is None:
        return fail("%s: no PRINT_START" % args.raw)
    # Where PRINT_START leaves the toolhead. Klipper is already there and drops the move; the slicer's
    # estimate, which cannot see into the macro, moves there before the first measured layer, so both
    # sides start the print from the same place.
    try:
        x, y, z = (float(v) for v in args.start_position.split(","))
    except ValueError:
        return fail("--start-position needs X,Y,Z, got %r" % args.start_position)
    lines.insert(start + 1, "G1 X%g Y%g Z%g" % (x, y, z))
    if args.max_layers is not None:
        cut = next((i for i, line in enumerate(lines)
                    if (m := LAYER_RE.match(line)) and int(m.group(1)) == args.max_layers + 1), None)
        end = next((i for i, line in enumerate(lines) if command_of(line) == "PRINT_END"), None)
        if cut is None or end is None or end < cut:
            return fail("%s: cannot cut after layer %d (no layer %d marker or no PRINT_END)"
                        % (args.raw, args.max_layers, args.max_layers + 1))
        # The end sequence (retract, PRINT_END, fan off) is kept so the file ends as a print does.
        lines = lines[:cut] + lines[end:]
    if args.legacy_limits:
        # What the legacy motion ownership emitted on every acceleration change: the deprecated
        # ACCEL_TO_DECEL at half the acceleration and a square corner velocity of 12.
        def legacy(line):
            m = ACCEL_RE.match(line)
            if not m:
                return line
            accel = float(m.group(1))
            return "SET_VELOCITY_LIMIT ACCEL=%s ACCEL_TO_DECEL=%s SQUARE_CORNER_VELOCITY=12" % (
                m.group(1), ("%.4f" % (accel / 2.)).rstrip("0").rstrip("."))
        lines = [legacy(line) for line in lines]
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return 0


def cmd_reference(args):
    markers = [m.strip().upper() for m in args.markers.split(",") if m.strip()]
    if not markers:
        return fail("--markers is empty")
    models = []
    for name in sorted(os.listdir(args.corpus)):
        if not name.endswith(".gcode"):
            continue
        stem = name[:-len(".gcode")]
        result_path = os.path.join(args.results, stem + ".json")
        if not os.path.isfile(result_path):
            return fail("no Klipper result for %s (%s)" % (name, result_path))
        with open(result_path, encoding="utf-8") as f:
            result = json.load(f)
        if not result.get("accepted"):
            return fail("Klipper rejected %s; see %s" % (name, result_path))
        with open(os.path.join(args.corpus, name), encoding="utf-8") as f:
            lines = f.read().splitlines()
        file_markers = [(i + 1, line) for i, line in enumerate(lines) if command_of(line) in markers]
        klipper_markers = result.get("markers", [])
        if len(file_markers) != len(klipper_markers):
            return fail("%s: %d marker lines in the file, %d timed by Klipper"
                        % (name, len(file_markers), len(klipper_markers)))
        entries = []
        for (line_no, line), timed in zip(file_markers, klipper_markers):
            if command_of(line) != timed["command"] or timed["time_s"] is None:
                return fail("%s line %d: '%s' does not match Klipper marker %s"
                            % (name, line_no, line, timed))
            entries.append({"line": line_no, "command": line, "time_s": timed["time_s"]})
        models.append({"name": stem, "gcode": name, "markers": entries})
    if not models:
        return fail("no .gcode files in %s" % args.corpus)
    reference = {
        "klipper": args.klipper_version,
        "printer_cfg": "tests/virtual-printer/printer.cfg",
        "marker_commands": markers,
        "models": models,
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(reference, f, indent=1)
        f.write("\n")
    return 0


def cmd_compare(args):
    with open(args.a, encoding="utf-8") as f:
        a = json.load(f)
    with open(args.b, encoding="utf-8") as f:
        b = json.load(f)
    ma, mb = a.get("markers", []), b.get("markers", [])
    if len(ma) != len(mb):
        return fail("marker count differs: %d vs %d" % (len(ma), len(mb)))
    worst = 0.
    for x, y in zip(ma, mb):
        if x["time_s"] is None or y["time_s"] is None:
            return fail("untimed marker %s / %s" % (x, y))
        worst = max(worst, abs(x["time_s"] - y["time_s"]))
    print("largest marker time difference: %.6f s" % worst)
    return 0 if worst <= args.tolerance else fail("runs differ by %.6f s > %.6f s" % (worst, args.tolerance))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("normalize")
    p.add_argument("raw")
    p.add_argument("out")
    p.add_argument("--start-position", required=True)
    p.add_argument("--max-layers", type=int)
    p.add_argument("--legacy-limits", action="store_true")
    p = sub.add_parser("reference")
    p.add_argument("--corpus", required=True)
    p.add_argument("--results", required=True)
    p.add_argument("--markers", required=True)
    p.add_argument("--klipper-version", required=True)
    p.add_argument("--out", required=True)
    p = sub.add_parser("compare")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--tolerance", type=float, required=True)
    args = parser.parse_args()
    return {"normalize": cmd_normalize, "reference": cmd_reference, "compare": cmd_compare}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
