#!/usr/bin/env bash
# KLIPSLICE virtual printer check: slice a calibration cube with the shipped
# Voron 2.4 350 presets, run the G-code through upstream Klipper in batch mode,
# then upload and print it through upstream Moonraker on a virtual Klippy.
# Everything runs in local Docker containers on an internal network; no real
# printer is ever contacted.
#
# usage: tests/virtual-printer/run.sh
#   KLIPSLICE_BIN   slicer binary; default: the macOS app bundle on Darwin,
#                   build/package/bin/klipslice on Linux
#   VP_KEEP_RIG=1   leave the Klipper/Moonraker containers running afterwards
#
# Writes tests/virtual-printer/out/ (report.md, G-code, logs, JSON results).
# Exit status: 0 all steps passed, 1 a step failed.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$HERE/../.." && pwd)"
OUT="$HERE/out"
PROFILES="$REPO/resources/profiles"
MODEL="$REPO/tests/data/20mm_cube.obj"
GCODE_NAME="klipslice_voron24_cube.gcode"
DICT=/opt/klipper-dict/linux-host.dict
PRINTER_PRESET="$PROFILES/Voron/machine/Voron 2.4 350 0.4 nozzle.json"
PROCESS_PRESET="$PROFILES/Voron/process/0.20mm Standard @Voron.json"
FILAMENT_PRESET="$PROFILES/OrcaFilamentLibrary/filament/Generic PLA @System.json"
# Seconds the accelerated virtual print may take (the cube runs in a few).
PRINT_TIMEOUT=300

log() { printf '[virtual-printer] %s\n' "$*"; }
die() { printf '[virtual-printer] ERROR: %s\n' "$*" >&2; exit 1; }

if [ -n "${KLIPSLICE_BIN:-}" ]; then
    BIN="$KLIPSLICE_BIN"
else
    case "$(uname -s)" in
        Darwin) BIN="$REPO/build/arm64/src/Release/KLIPSLICE.app/Contents/MacOS/KLIPSLICE" ;;
        Linux) BIN="$REPO/build/package/bin/klipslice" ;;
        *) die "unsupported host $(uname -s); set KLIPSLICE_BIN" ;;
    esac
fi
[ -x "$BIN" ] || die "slicer binary not found or not executable: $BIN"
for f in "$MODEL" "$PRINTER_PRESET" "$PROCESS_PRESET" "$FILAMENT_PRESET"; do
    [ -f "$f" ] || die "missing input: $f"
done
command -v docker > /dev/null 2>&1 || die "docker not found"
docker compose version > /dev/null 2>&1 || die "docker compose (v2) not available"

# The containers run as the invoking user so files in out/ stay owned by it.
VP_UID="$(id -u)"
VP_GID="$(id -g)"
export VP_UID VP_GID
compose() { docker compose --project-directory "$HERE" -f "$HERE/docker-compose.yml" "$@"; }

rm -rf "$OUT"
mkdir -p "$OUT/slice"

trap '[ "${VP_KEEP_RIG:-0}" = "1" ] || compose down -v --remove-orphans > /dev/null 2>&1 || true' EXIT

# --- 1. Slice ---------------------------------------------------------------
log "slicing $(basename "$MODEL") with $BIN"
TIMEOUT=()
if command -v timeout > /dev/null 2>&1; then
    TIMEOUT=(timeout 600)
fi
slice_rc=0
${TIMEOUT[@]+"${TIMEOUT[@]}"} "$BIN" --datadir "$OUT/slice/datadir" \
    --load-settings "$PRINTER_PRESET;$PROCESS_PRESET" \
    --load-filaments "$FILAMENT_PRESET" \
    --slice 0 --outputdir "$OUT/slice" "$MODEL" > "$OUT/slice.log" 2>&1 || slice_rc=$?
rm -rf "$OUT/slice/datadir"
if [ "$slice_rc" -ne 0 ] || [ ! -s "$OUT/slice/plate_1.gcode" ]; then
    tail -n 40 "$OUT/slice.log" >&2
    die "slicing failed (exit $slice_rc)"
fi
mv "$OUT/slice/plate_1.gcode" "$OUT/slice/$GCODE_NAME"
log "G-code: out/slice/$GCODE_NAME"

# --- Image ------------------------------------------------------------------
log "building the rig image (pinned Klipper and Moonraker)"
compose build --quiet || die "image build failed (network fetch or compile error)"
compose down -v --remove-orphans > /dev/null 2>&1 || die "could not reset the rig"

# --- 2. Klipper batch mode --------------------------------------------------
log "running the G-code through Klipper batch mode"
klipper_rc=0
compose run --rm --no-deps klipper-batch \
    --config /vp/printer.cfg --gcode "/out/slice/$GCODE_NAME" --dict "$DICT" \
    --log /out/klippy-batch.log --summary /out/klipper.json || klipper_rc=$?
[ -f "$OUT/klipper.json" ] || die "Klipper batch run produced no result (exit $klipper_rc)"

# --- 3. Moonraker -----------------------------------------------------------
log "starting virtual Klippy and Moonraker"
moonraker_rc=0
if compose up -d --wait --wait-timeout 180 klipper moonraker; then
    log "uploading and printing through Moonraker"
    compose exec -T moonraker /opt/moonraker-env/bin/python /vp/check_moonraker.py \
        --gcode "/out/slice/$GCODE_NAME" --remote-name "$GCODE_NAME" \
        --result /out/moonraker.json --print-timeout "$PRINT_TIMEOUT" || moonraker_rc=$?
else
    moonraker_rc=1
    log "rig did not become healthy"
fi
compose logs --no-color klipper moonraker > "$OUT/rig-containers.log" 2>&1 || true
compose exec -T klipper cat /printer_data/logs/klippy.log > "$OUT/klippy-live.log" 2>/dev/null || true
compose exec -T moonraker cat /printer_data/logs/moonraker.log > "$OUT/moonraker.log" 2>/dev/null || true

# --- Report -----------------------------------------------------------------
compose run --rm --no-deps --entrypoint /opt/klippy-env/bin/python klipper-batch /vp/report.py \
    --gcode "/out/slice/$GCODE_NAME" --binary "$BIN" --model "tests/data/$(basename "$MODEL")" \
    --presets "Voron 2.4 350 0.4 nozzle; 0.20mm Standard @Voron; Generic PLA @System" \
    --klipper /out/klipper.json --moonraker /out/moonraker.json --out /out/report.md \
    || die "report generation failed"
log "report: $OUT/report.md"

status=0
[ "$klipper_rc" -eq 0 ] || { log "FAIL: Klipper rejected the G-code (exit $klipper_rc)"; status=1; }
[ "$moonraker_rc" -eq 0 ] || { log "FAIL: Moonraker step failed (exit $moonraker_rc)"; status=1; }
[ "$status" -eq 0 ] && log "PASS"
exit "$status"
