#!/usr/bin/env bash
# End-to-end check that the CLI keeps a project whose system presets are not shipped.
#
# ANANAS SLICER ships no Bambu Lab (BBL) profiles, but users still open projects saved by Bambu Studio or
# upstream OrcaSlicer that name BBL printer, process and filament presets. Such a project must slice
# and re-export with its embedded settings: the unresolved presets are logged, never fatal, and never
# a crash. A project is exported from the shipped Voron 2.4 350 presets, its preset identity is
# rewritten to BBL system presets that do not exist here, sentinel values are planted in printer,
# process and filament keys, and it is sliced again, once plain and once through --arrange with
# --allow-rotations (the two upstream orca-test-repo shapes that load such projects).
#
# usage: test_cli_project_unknown_presets.sh <orca-slicer binary> <python3> <resources/profiles>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }
if [ -e "$PROFILES/BBL.json" ] || [ -d "$PROFILES/BBL" ]; then
    echo "FAIL: $PROFILES ships a BBL vendor; this test asserts behaviour without it"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-unknown-presets.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# GNU timeout is not part of macOS; run unbounded there (ctest's TIMEOUT still applies).
TIMEOUT=()
if command -v timeout > /dev/null 2>&1; then
    TIMEOUT=(timeout 300)
fi

"$PY" - "$WORK/cube.stl" <<'EOF' || { echo "FAIL: could not write cube.stl"; exit 1; }
import sys

v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF

# run <tag> <input> [option...]: run into $WORK/<tag>/ with a fresh data directory. A signal (exit code
# above 128, e.g. 134 SIGABRT or 139 SIGSEGV) is reported as a crash so it is not mistaken for a clean
# CLI error code.
run() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    ${TIMEOUT[@]+"${TIMEOUT[@]}"} "$BIN" --datadir "$out/datadir" --outputdir "$out" "$@" "$input" > "$out/log" 2>&1
    local status=$?
    if [ "$status" -gt 128 ] && [ "$status" -ne 124 ]; then
        echo "FAIL: $1: orca-slicer crashed (signal $((status - 128)))"; tail -n 40 "$out/log"; exit 1
    fi
    [ "$status" -eq 0 ] || { echo "FAIL: $1: orca-slicer exited $status"; tail -n 40 "$out/log"; exit 1; }
}

run base "$WORK/cube.stl" --slice 0 --export-3mf out.3mf \
    --load-settings "$PROFILES/Voron/machine/Voron 2.4 350 0.4 nozzle.json;$PROFILES/Voron/process/0.20mm Standard @Voron.json" \
    --load-filaments "$PROFILES/OrcaFilamentLibrary/filament/Generic PLA @System.json"

# Rewrite the preset identity to BBL system presets and plant sentinel values the project must keep.
"$PY" - "$WORK/base/out.3mf" "$WORK/bbl.3mf" <<'EOF' || exit $?
import json, sys, zipfile

src, dst = sys.argv[1], sys.argv[2]
printer, process, filament = "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C"
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            filaments = len(config["filament_settings_id"])
            config["printer_settings_id"] = printer
            config["print_settings_id"] = process
            config["filament_settings_id"] = [filament] * filaments
            config["printer_model"] = "Bambu Lab X1 Carbon"
            # inherits_group: [process, filament..., printer]
            config["inherits_group"] = [process] + [filament] * filaments + [printer]
            config["print_compatible_printers"] = [printer]
            config["upward_compatible_machine"] = []
            expected = {
                "printer_settings_id": printer,
                "print_settings_id": process,
                "wall_loops": str(int(config["wall_loops"]) + 2),
                "top_shell_layers": str(int(config["top_shell_layers"]) + 1),
                "printable_height": "%g" % (float(config["printable_height"]) - 17),
                "nozzle_temperature": [str(int(t) + 3) for t in config["nozzle_temperature"]],
            }
            for key, value in expected.items():
                config[key] = value
            data = json.dumps(config, indent=4)
        zout.writestr(item, data)
with open(dst + ".expected.json", "w") as f:
    json.dump(expected, f)
EOF

run project "$WORK/bbl.3mf" --allow-newer-file --slice 0 --export-3mf out.3mf
run arrange "$WORK/bbl.3mf" --allow-newer-file --allow-rotations --arrange 1 --export-3mf out.3mf

"$PY" - "$WORK" "$WORK/bbl.3mf.expected.json" <<'EOF'
import json, os, sys, zipfile

work, expected_path = sys.argv[1], sys.argv[2]
with open(expected_path) as f:
    expected = json.load(f)
errors = []
if not os.path.isfile(os.path.join(work, "project", "plate_1.gcode")):
    errors.append("project: plate_1.gcode was not written")
for tag in ("project", "arrange"):
    path = os.path.join(work, tag, "out.3mf")
    if not os.path.isfile(path):
        errors.append("%s: out.3mf was not written" % tag)
        continue
    with zipfile.ZipFile(path) as z:
        config = json.loads(z.read("Metadata/project_settings.config"))
    errors += ["%s: %s is %r, want %r" % (tag, key, config.get(key), want)
               for key, want in expected.items() if config.get(key) != want]
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF
status=$?
[ "$status" -eq 0 ] || { tail -n 40 "$WORK/project/log"; exit 1; }
echo "PASS"
