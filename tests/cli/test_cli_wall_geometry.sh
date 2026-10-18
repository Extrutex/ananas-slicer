#!/usr/bin/env bash
# End-to-end wall-geometry check on the real binary and the shipped Voron presets.
#
# Generates a fin row (0.15 to 1.2 mm) and a wedge (0.05 to 2.0 mm) as STL, slices them with
# Voron 2.4 350 0.4 nozzle / 0.20mm Standard @Voron / Generic PLA with precise_outer_wall off, on with the
# shipped method (outline_shrink) and on with precise_outer_wall_method = toolpath_shift, and reads the G-code
# of the layer at Z=1.0 (WIDTH and E per move).
# The bands come from the rounded-rectangle bead model, not from earlier output:
#   - every fin at least min_feature_size thick is extruded;
#   - a single-bead fin is max(t, min_bead_width + c) wide, +-0.005 mm, where c = layer_height * (1 - pi/4)
#     (criterion Q1 of the wall analysis; the shipped process uses Arachne);
#   - a fin printed as one bead with precise_outer_wall off is one bead of the same width, +-0.005 mm, with
#     it on, by either method (the second half of Q1);
#   - the wedge is extruded down to min_feature_size + 0.05 mm.
# Checks that fail on the current code for a known defect are listed in KNOWN below. They are expected to
# fail; if one passes, the script fails too, so the fixing commit has to remove it from the list.
#
# usage: test_cli_wall_geometry.sh <orca-slicer binary> <python3> <resources/profiles>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-wall-geometry.XXXXXX")" || exit 1
trap 'rm -rf "$WORK"' EXIT

# Known defects: "<variant>:<check>" (WALL-1: the outline_shrink method shrinks the outline before beading).
KNOWN="precise1:presence:0.15 precise1:width:0.40 precise1:width:0.45 precise1:width:0.50 precise1:width:0.60 precise1:wedge
parity_precise1:0.15 parity_precise1:0.40 parity_precise1:0.45 parity_precise1:0.50 parity_precise1:0.60"

# Fins along X at Y 0..12, the wedge along X at Y 20..22 (thin end at X=0).
"$PY" - "$WORK/walls.stl" <<'EOF' || exit 1
import sys

FINS = [0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2]

def prism(poly, h):
    # Convex polygon, counter-clockwise, extruded from z=0 to z=h.
    n = len(poly)
    cx = sum(p[0] for p in poly) / n
    cy = sum(p[1] for p in poly) / n
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        yield (cx, cy, 0), (b[0], b[1], 0), (a[0], a[1], 0)
        yield (cx, cy, h), (a[0], a[1], h), (b[0], b[1], h)
        yield (a[0], a[1], 0), (b[0], b[1], 0), (b[0], b[1], h)
        yield (a[0], a[1], 0), (b[0], b[1], h), (a[0], a[1], h)

tris = []
x = 0.0
for t in FINS:
    tris += prism([(x, 0), (x + t, 0), (x + t, 12), (x, 12)], 2.0)
    x += t + 3
tris += prism([(0, 21 - 0.025), (40, 20), (40, 22), (0, 21 + 0.025)], 2.0)
with open(sys.argv[1], "w") as f:
    f.write("solid walls\n")
    for tri in tris:
        f.write("facet normal 0 0 0\nouter loop\n")
        for p in tri:
            f.write("vertex %.6f %.6f %.6f\n" % p)
        f.write("endloop\nendfacet\n")
    f.write("endsolid walls\n")
EOF

# The shipped process, flattened into a standalone preset with precise_outer_wall and its method set.
flatten() {
    "$PY" - "$PROFILES/Voron/process" "0.20mm Standard @Voron" "$1" "$2" "$3" <<'EOF' || exit 1
import json, os, sys

folder, name, precise, method, out = sys.argv[1:6]

def load(n):
    with open(os.path.join(folder, n + ".json")) as f:
        d = json.load(f)
    if "inherits" in d:
        base = load(d.pop("inherits"))
        base.update(d)
        d = base
    return d

d = load(name)
d.update({"type": "process", "from": "User", "instantiation": "true", "name": "wall geometry precise=%s %s" % (precise, method),
          "precise_outer_wall": precise, "precise_outer_wall_method": method})
with open(out, "w") as f:
    json.dump(d, f)
EOF
}

# macOS ships no timeout(1); ctest's TIMEOUT still bounds the run there.
TIMEOUT=()
command -v timeout > /dev/null && TIMEOUT=(timeout 300)

fails=0
# variant:precise_outer_wall:precise_outer_wall_method
for spec in precise0:0:outline_shrink precise1:1:outline_shrink shift:1:toolpath_shift; do
    IFS=: read -r variant precise method <<< "$spec"
    out="$WORK/$variant"
    mkdir -p "$out"
    flatten "$precise" "$method" "$WORK/process_$variant.json"
    ${TIMEOUT[@]+"${TIMEOUT[@]}"} "$BIN" --datadir "$out/datadir" \
        --load-settings "$PROFILES/Voron/machine/Voron 2.4 350 0.4 nozzle.json;$WORK/process_$variant.json" \
        --load-filaments "$PROFILES/OrcaFilamentLibrary/filament/Generic PLA @System.json" \
        --arrange 1 --slice 0 --outputdir "$out" "$WORK/walls.stl" > "$out/log" 2>&1 \
        || { echo "FAIL: $variant: slicer exited $?"; tail -n 40 "$out/log"; exit 1; }
    gcode="$(ls "$out"/*.gcode 2>/dev/null | head -n 1)"
    [ -n "$gcode" ] || { echo "FAIL: $variant: no G-code"; exit 1; }
    # The G-code records the method it was sliced with.
    grep -q "^; precise_outer_wall_method = $method\$" "$gcode" \
        || { echo "FAIL: $variant: the G-code does not record precise_outer_wall_method = $method"; fails=$((fails + 1)); }

    "$PY" - "$gcode" "$variant" "$WORK/process_$variant.json" "$KNOWN" "$WORK/widths_$variant.json" <<'EOF'
import json, math, sys

gcode, variant, process, known, widths_out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(), sys.argv[5]
FINS = [0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2]
PROBE_Z, TOL, WEDGE_ALLOWANCE = 1.0, 0.005, 0.05

with open(process) as f:
    cfg = json.load(f)

def pct(key, default):
    v = str(cfg.get(key, default))
    return float(v.rstrip("%")) / 100 if v.endswith("%") else float(v) / 0.4

h = float(cfg["layer_height"])
c = h * (1 - math.pi / 4)
nozzle = 0.4
min_feature = pct("min_feature_size", "25%") * nozzle
floor = pct("min_bead_width", "85%") * nozzle + c

# Extruding moves of the probe layer: (x0, y0, x1, y1, width).
segs, x, y, z, w = [], 0.0, 0.0, None, 0.0
with open(gcode) as f:
    for line in f:
        if line.startswith(";Z:"):
            z = float(line[3:])
        elif line.startswith(";WIDTH:"):
            w = float(line[7:])
        elif line.startswith(("G1 ", "G0 ")):
            d = {t[0]: float(t[1:]) for t in line.split(";")[0].split()[1:] if len(t) > 1}
            nx, ny = d.get("X", x), d.get("Y", y)
            if z is not None and abs(z - PROBE_Z) < 1e-6 and d.get("E", 0) > 0 and (nx, ny) != (x, y):
                segs.append((x, y, nx, ny, w))
            x, y = nx, ny
if not segs:
    sys.exit("FAIL: %s: no extrusion at Z=%.1f" % (variant, PROBE_Z))

# The fin row lies below the wedge. Anchor X on the 1.2 mm fin, the right end of the row, whose outer
# beads sit w/2 inside its edges.
ymin = min(min(s[1], s[3]) for s in segs)
fins = [s for s in segs if max(s[1], s[3]) < ymin + 15]
wedge = [s for s in segs if min(s[1], s[3]) >= ymin + 15]
right = max(max(s[0], s[2]) + s[4] / 2 for s in fins)
edges, x0 = [], right - sum(FINS) - 3 * (len(FINS) - 1)
for t in FINS:
    edges.append((x0, x0 + t))
    x0 += t + 3
ymid = (min(min(s[1], s[3]) for s in fins) + max(max(s[1], s[3]) for s in fins)) / 2

results = []  # (check id, pass, message)
widths = {}   # fin -> widths of the beads crossing its middle, for the parity check
for t, (a, b) in zip(FINS, edges):
    mine = [s for s in fins if a - 1.0 < (s[0] + s[2]) / 2 < b + 1.0]
    if t >= min_feature:
        results.append(("presence:%.2f" % t, bool(mine), "fin %.2f extruded: %s" % (t, bool(mine))))
    cross = []
    for s in mine:
        if (s[1] <= ymid < s[3]) or (s[3] <= ymid < s[1]):
            cross.append(s[4])
    widths["%.2f" % t] = cross
    if len(cross) == 1:
        lo, hi = max(t, floor) - TOL, max(t, floor) + TOL
        results.append(("width:%.2f" % t, lo <= cross[0] <= hi,
                        "fin %.2f single bead width %.3f in [%.3f, %.3f]" % (t, cross[0], lo, hi)))

if wedge:
    thin_x = min(min(s[0], s[2]) for s in wedge)
    thick_edge = max(max(s[0], s[2]) + s[4] / 2 for s in wedge)
    cutoff = 0.05 + 1.95 * (thin_x - (thick_edge - 40)) / 40
else:
    cutoff = 2.0
limit = min_feature + WEDGE_ALLOWANCE
results.append(("wedge", cutoff <= limit, "wedge extruded down to %.3f mm, limit %.3f" % (cutoff, limit)))

with open(widths_out, "w") as f:
    json.dump(widths, f)

failed = 0
for cid, ok, msg in results:
    key = variant + ":" + cid
    if key in known:
        if ok:
            print("FAIL: %s: %s, but it is listed as a known failure; remove it from KNOWN" % (variant, msg))
            failed += 1
        else:
            print("XFAIL: %s: %s" % (variant, msg))
    elif ok:
        print("ok: %s: %s" % (variant, msg))
    else:
        print("FAIL: %s: %s" % (variant, msg))
        failed += 1
listed = [k for k in known if k.startswith(variant + ":")]
for k in listed:
    if k[len(variant) + 1:] not in [r[0] for r in results]:
        print("FAIL: known failure %s matches no check" % k)
        failed += 1
sys.exit(1 if failed else 0)
EOF
    [ $? -eq 0 ] || fails=$((fails + 1))
done

# Parity: precise_outer_wall must not change a single bead, by either method.
for variant in precise1 shift; do
    "$PY" - "$WORK/widths_precise0.json" "$WORK/widths_$variant.json" "$KNOWN" "parity_$variant" <<'EOF' || fails=$((fails + 1))
import json, sys

TOL = 0.005
with open(sys.argv[1]) as f:
    off = json.load(f)
with open(sys.argv[2]) as f:
    on = json.load(f)
known, name = sys.argv[3].split(), sys.argv[4]

failed, checked = 0, []
for fin, a in off.items():
    if len(a) != 1:
        continue
    b = on.get(fin, [])
    cid = name + ":" + fin
    checked.append(cid)
    ok = len(b) == 1 and abs(b[0] - a[0]) <= TOL
    msg = "fin %s single bead %.3f off, %s on" % (fin, a[0], "%.3f" % b[0] if len(b) == 1 else "%d beads" % len(b))
    if cid in known:
        if ok:
            print("FAIL: %s: %s, but it is listed as a known failure; remove it from KNOWN" % (name, msg))
            failed += 1
        else:
            print("XFAIL: %s: %s" % (name, msg))
    elif ok:
        print("ok: %s: %s" % (name, msg))
    else:
        print("FAIL: %s: %s" % (name, msg))
        failed += 1
for k in known:
    if k.startswith(name + ":") and k not in checked:
        print("FAIL: known failure %s matches no check" % k)
        failed += 1
sys.exit(1 if failed else 0)
EOF
done

[ "$fails" -eq 0 ] || exit 1
echo "PASS"
