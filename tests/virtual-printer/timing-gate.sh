#!/usr/bin/env bash
# KLIPSLICE Klipper timing gate: checks the slicer's Klipper time model against
# upstream Klipper batch mode.
#
# 1. Slices the timing corpus with the shipped Voron 2.4 350 presets and reduces
#    each slice to the commands Klipper executes (timing_corpus.py normalize).
# 2. Runs every corpus file through pinned Klipper in batch mode against
#    printer.cfg and records the exact print time at each marker command
#    (klippy_batch.py --markers), twice for the first file to check that Klipper's
#    timing is reproducible.
# 3. Writes reference.json and runs the [KlipperTimingGate] test of
#    fff_print_tests on the fresh corpus, which fails when the estimate is off by
#    more than its tolerance.
#
# usage: tests/virtual-printer/timing-gate.sh [--update]
#   --update            also replace the committed corpus in tests/data/klipper_timing
#   KLIPSLICE_BIN       slicer binary (default: as in run.sh)
#   KLIPSLICE_BUILD_DIR build tree for the default slicer binary and for
#                       fff_print_tests (default: build/arm64 on Darwin, build on Linux)
#   FFF_PRINT_TESTS     fff_print_tests binary (default: from KLIPSLICE_BUILD_DIR)
#
# Writes tests/virtual-printer/out/timing/. Exit status: 0 gate passed, 1 failed, 2 usage.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$HERE/../.." && pwd)"
OUT="$HERE/out/timing"
COMMITTED="$REPO/tests/data/klipper_timing"
PROFILES="$REPO/resources/profiles"
DICT=/opt/klipper-dict/linux-host.dict
PRINTER_PRESET="$PROFILES/Voron/machine/Voron 2.4 350 0.4 nozzle.json"
FILAMENT_PRESET="$PROFILES/OrcaFilamentLibrary/filament/Generic PLA @System.json"
STANDARD="$PROFILES/Voron/process/0.20mm Standard @Voron.json"
FINE="$PROFILES/Voron/process/0.12mm Fine @Voron.json"
# Marker commands timed on both sides. The gate measures from
# SET_PRINT_STATS_INFO CURRENT_LAYER=1 to PRINT_END (whole print) and to
# CURRENT_LAYER=2 (first layer).
MARKERS="SET_PRINT_STATS_INFO,EXCLUDE_OBJECT_START,EXCLUDE_OBJECT_END,PRINT_END"
# Where printer.cfg's PRINT_START leaves the toolhead: safe_z_home at 175,175,
# then G1 Z10.
START_POSITION="175,175,10"
# Klipper's motion timing has no randomness; two runs must agree to this.
DETERMINISM_TOLERANCE_S=0.1

# name|model|process preset|normalize options
# Layer cuts keep the committed corpus small; every file still starts with a
# full first layer and ends with the print's end sequence.
CORPUS=(
    "cube|20mm_cube.obj|$STANDARD|"
    "cube_fine|20mm_cube.obj|$FINE|--max-layers 40"
    "cube_legacy_limits|20mm_cube.obj|$STANDARD|--legacy-limits --max-layers 30"
    "extruder_idler|extruder_idler.obj|$STANDARD|--max-layers 25"
    "frog_legs|frog_legs.obj|$STANDARD|--max-layers 4"
)

log() { printf '[timing-gate] %s\n' "$*"; }
die() { printf '[timing-gate] ERROR: %s\n' "$*" >&2; exit 1; }

UPDATE=0
case "${1:-}" in
    "") ;;
    --update) UPDATE=1 ;;
    *) printf 'usage: %s [--update]\n' "$0" >&2; exit 2 ;;
esac

case "$(uname -s)" in
    Darwin) BUILD_DIR="${KLIPSLICE_BUILD_DIR:-build/arm64}" ;;
    Linux) BUILD_DIR="${KLIPSLICE_BUILD_DIR:-build}" ;;
    *) die "unsupported host $(uname -s); set KLIPSLICE_BIN and FFF_PRINT_TESTS" ;;
esac
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$REPO/$BUILD_DIR" ;;
esac
if [ -n "${KLIPSLICE_BIN:-}" ]; then
    BIN="$KLIPSLICE_BIN"
else
    case "$(uname -s)" in
        Darwin) BIN="$BUILD_DIR/src/Release/KLIPSLICE.app/Contents/MacOS/KLIPSLICE" ;;
        Linux) BIN="$BUILD_DIR/package/bin/klipslice" ;;
    esac
fi
if [ -n "${FFF_PRINT_TESTS:-}" ]; then
    TESTS="$FFF_PRINT_TESTS"
else
    case "$(uname -s)" in
        Darwin) TESTS="$BUILD_DIR/tests/fff_print/Release/fff_print_tests.app/Contents/MacOS/fff_print_tests" ;;
        Linux) TESTS="$BUILD_DIR/tests/fff_print/Release/fff_print_tests" ;;
    esac
fi
[ -x "$BIN" ] || die "slicer binary not found or not executable: $BIN"
[ -x "$TESTS" ] || die "fff_print_tests not found or not executable: $TESTS"
for f in "$PRINTER_PRESET" "$FILAMENT_PRESET" "$STANDARD" "$FINE"; do
    [ -f "$f" ] || die "missing preset: $f"
done
command -v python3 > /dev/null 2>&1 || die "python3 not found"
command -v docker > /dev/null 2>&1 || die "docker not found"
docker compose version > /dev/null 2>&1 || die "docker compose (v2) not available"

VP_UID="$(id -u)"
VP_GID="$(id -g)"
export VP_UID VP_GID
compose() { docker compose --project-directory "$HERE" -f "$HERE/docker-compose.yml" "$@"; }
trap 'compose down -v --remove-orphans > /dev/null 2>&1 || true' EXIT

rm -rf "$OUT"
mkdir -p "$OUT/raw" "$OUT/corpus" "$OUT/klipper"

# --- 1. Slice and normalize ---------------------------------------------------
for entry in "${CORPUS[@]}"; do
    IFS='|' read -r name model process options <<< "$entry"
    log "slicing $name ($model, $(basename "$process"))"
    slice_dir="$OUT/raw/$name"
    mkdir -p "$slice_dir"
    "$BIN" --datadir "$slice_dir/datadir" --load-settings "$PRINTER_PRESET;$process" \
        --load-filaments "$FILAMENT_PRESET" --slice 0 --outputdir "$slice_dir" \
        "$REPO/tests/data/$model" > "$slice_dir/slice.log" 2>&1 \
        || die "slicing $name failed; see $slice_dir/slice.log"
    rm -rf "$slice_dir/datadir"
    [ -s "$slice_dir/plate_1.gcode" ] || die "slicing $name produced no G-code"
    # shellcheck disable=SC2086 # options is a word list by design
    python3 -I "$HERE/timing_corpus.py" normalize "$slice_dir/plate_1.gcode" "$OUT/corpus/$name.gcode" \
        --start-position "$START_POSITION" $options \
        || die "normalizing $name failed"
done

# --- 2. Klipper batch mode -----------------------------------------------------
log "building the rig image (pinned Klipper)"
compose build --quiet klipper-batch || die "image build failed"
KLIPPER_TAG="$(sed -n 's/^ARG KLIPPER_TAG=//p' "$HERE/Dockerfile")"
KLIPPER_COMMIT="$(compose run --rm --no-deps --entrypoint cat klipper-batch /opt/klipper/klippy/.version)" \
    || die "cannot read the Klipper version from the image"
KLIPPER_VERSION="$KLIPPER_TAG ($KLIPPER_COMMIT)"

run_klipper() {
    compose run --rm --no-deps klipper-batch --config /vp/printer.cfg --gcode "/out/timing/corpus/$1.gcode" \
        --dict "$DICT" --markers "$MARKERS" --log "/out/timing/klipper/$2.log" --summary "/out/timing/klipper/$2.json" \
        > /dev/null 2>&1
}
for entry in "${CORPUS[@]}"; do
    name="${entry%%|*}"
    log "Klipper batch: $name"
    run_klipper "$name" "$name" || die "Klipper rejected $name; see $OUT/klipper/$name.json"
done
first="${CORPUS[0]%%|*}"
run_klipper "$first" "$first.repeat" || die "repeat Klipper run of $first failed"
python3 -I "$HERE/timing_corpus.py" compare "$OUT/klipper/$first.json" "$OUT/klipper/$first.repeat.json" \
    --tolerance "$DETERMINISM_TOLERANCE_S" || die "Klipper timing is not reproducible"
rm -f "$OUT/klipper/$first.repeat.json" "$OUT/klipper/$first.repeat.log"

python3 -I "$HERE/timing_corpus.py" reference --corpus "$OUT/corpus" --results "$OUT/klipper" \
    --markers "$MARKERS" --klipper-version "$KLIPPER_VERSION" --out "$OUT/corpus/reference.json" \
    || die "building reference.json failed"

# --- 3. Gate ---------------------------------------------------------------------
log "running the [KlipperTimingGate] test on the fresh corpus"
gate_rc=0
KLIPPER_TIMING_DIR="$OUT/corpus" "$TESTS" "[KlipperTimingGate]" --success > "$OUT/gate.log" 2>&1 || gate_rc=$?
grep -E 'print .* s vs Klipper' "$OUT/gate.log" | sort -u || true

if [ "$UPDATE" -eq 1 ]; then
    rm -rf "$COMMITTED"
    mkdir -p "$COMMITTED"
    cp "$OUT/corpus/"*.gcode "$OUT/corpus/reference.json" "$COMMITTED/"
    log "updated $COMMITTED"
fi

if [ "$gate_rc" -ne 0 ]; then
    log "FAIL: the estimate is outside the gate tolerance; see $OUT/gate.log"
    exit 1
fi
log "PASS"
