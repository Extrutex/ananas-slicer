#!/usr/bin/env bash
# KLIPSLICE virtual printer check: slice a calibration cube with the shipped
# Voron 2.4 350 presets, run the G-code through upstream Klipper in batch mode,
# check the shipped print state macros in batch mode, then upload and print the
# G-code through upstream Moonraker on a virtual Klippy, and cancel a running
# print through Moonraker with Mainsail's CANCEL_PRINT.
# Everything runs in local Docker containers on an internal network; no real
# printer is ever contacted.
#
# usage: tests/virtual-printer/run.sh
#   KLIPSLICE_BIN   slicer binary; default: the app bundle under
#                   KLIPSLICE_BUILD_DIR on Darwin, its package/bin/klipslice on Linux
#   KLIPSLICE_BUILD_DIR  build tree the default binary is taken from (absolute or
#                   relative to the repository); default build/arm64 on Darwin,
#                   build on Linux. Ignored when KLIPSLICE_BIN is set.
#   VP_KEEP_RIG=1   leave the Klipper/Moonraker containers running afterwards
#
# Writes tests/virtual-printer/out/ (report.md, G-code, logs, JSON results).
# Exit status: 0 all steps passed, 1 a step failed.
set -euo pipefail
# Only step 5 picks its own printer.cfg; a value exported by the caller must not leak into the default rig.
unset VP_PRINTER_CFG

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
        Darwin) BUILD_DIR="${KLIPSLICE_BUILD_DIR:-build/arm64}" ;;
        Linux) BUILD_DIR="${KLIPSLICE_BUILD_DIR:-build}" ;;
        *) die "unsupported host $(uname -s); set KLIPSLICE_BIN" ;;
    esac
    case "$BUILD_DIR" in
        /*) ;;
        *) BUILD_DIR="$REPO/$BUILD_DIR" ;;
    esac
    case "$(uname -s)" in
        Darwin) BIN="$BUILD_DIR/src/Release/KLIPSLICE.app/Contents/MacOS/KLIPSLICE" ;;
        Linux) BIN="$BUILD_DIR/package/bin/klipslice" ;;
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

# --- 3. State macros -------------------------------------------------------
# resources/klipper/klipslice_state.cfg in Klipper batch mode: save, cancel and
# restore with an extruder without a stepper and an [extruder_stepper], once
# with the rig's own CANCEL_PRINT and once with Mainsail's and its cancel hook,
# with two extruder steppers whose names differ only in case, and with
# extruder steppers whose names hold a quote or a backslash; the check G-code
# raises a G-code error wherever Klipper's state is not the expected
# one.
log "checking the print state macros in Klipper batch mode"
macros_rc=0
# config:G-code:result name, all in state-macros/ and out/.
for route in printer:check:state-macros printer-mainsail:check-mainsail:state-macros-mainsail \
             printer-case:check-case:state-macros-case printer-names:check-names:state-macros-names; do
    IFS=: read -r cfg check result <<< "$route"
    rc=0
    compose run --rm --no-deps klipper-batch \
        --config "/vp/state-macros/$cfg.cfg" --gcode "/vp/state-macros/$check.gcode" --dict "$DICT" \
        --log "/out/$result.log" --summary "/out/$result.json" > /dev/null || rc=$?
    [ -f "$OUT/$result.json" ] || die "state macro check $check.gcode produced no result (exit $rc)"
    [ "$rc" -eq 0 ] || { log "state macro check $check.gcode failed (exit $rc)"; macros_rc=$rc; }
done
# The restore says why it skips the steppers whose names differ only in case.
for stepper in Belt belt; do
    grep -qF "_KLIPSLICE_RESTORE: [extruder_stepper $stepper] and another section differ only in case" \
        "$OUT/state-macros-case.log" \
        || { log "state macro check check-case.gcode: no console warning for [extruder_stepper $stepper]"; macros_rc=1; }
done
# It also says why it skips the steppers whose names it cannot pass unquoted.
for stepper in "Belt'aux" 'Belt\aux'; do
    grep -qF "_KLIPSLICE_RESTORE: the name of [extruder_stepper $stepper] holds characters other than" \
        "$OUT/state-macros-names.log" \
        || { log "state macro check check-names.gcode: no console warning for [extruder_stepper $stepper]"; macros_rc=1; }
done

# --- 4. Moonraker -----------------------------------------------------------
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

# --- 5. Cancel a running print ----------------------------------------------
# The batch routes of step 3 call CANCEL_PRINT without a print running, so they
# would not notice a cancel that leaves the print running. Here the
# long-running Klippy gets state-macros/printer-cancel.cfg (Mainsail's
# CANCEL_PRINT with the _KLIPSLICE_RESTORE hook), prints
# state-macros/cancel.gcode through Moonraker and cancels it mid-print.
log "cancelling a running print through Moonraker (Mainsail's CANCEL_PRINT)"
cancel_rc=0
compose down -v --remove-orphans > /dev/null 2>&1 || die "could not reset the rig"
if VP_PRINTER_CFG=state-macros/printer-cancel.cfg compose up -d --wait --wait-timeout 180 klipper moonraker; then
    compose exec -T moonraker /opt/moonraker-env/bin/python /vp/check_cancel.py \
        --gcode /vp/state-macros/cancel.gcode --remote-name klipslice_vp_cancel.gcode \
        --result /out/cancel.json || cancel_rc=$?
else
    cancel_rc=1
    log "rig with printer-cancel.cfg did not become healthy"
fi
compose logs --no-color klipper moonraker > "$OUT/cancel-containers.log" 2>&1 || true
compose exec -T klipper cat /printer_data/logs/klippy.log > "$OUT/klippy-cancel.log" 2>/dev/null || true
compose exec -T moonraker cat /printer_data/logs/moonraker.log > "$OUT/moonraker-cancel.log" 2>/dev/null || true

# --- Report -----------------------------------------------------------------
compose run --rm --no-deps --entrypoint /opt/klippy-env/bin/python klipper-batch /vp/report.py \
    --gcode "/out/slice/$GCODE_NAME" --binary "$BIN" --model "tests/data/$(basename "$MODEL")" \
    --presets "Voron 2.4 350 0.4 nozzle; 0.20mm Standard @Voron; Generic PLA @System" \
    --klipper /out/klipper.json --state-macros /out/state-macros.json /out/state-macros-mainsail.json /out/state-macros-case.json /out/state-macros-names.json \
    --moonraker /out/moonraker.json --cancel /out/cancel.json --out /out/report.md \
    || die "report generation failed"
log "report: $OUT/report.md"

status=0
[ "$klipper_rc" -eq 0 ] || { log "FAIL: Klipper rejected the G-code (exit $klipper_rc)"; status=1; }
[ "$macros_rc" -eq 0 ] || { log "FAIL: the state macros left Klipper in the wrong state (exit $macros_rc, see out/state-macros*.json)"; status=1; }
[ "$moonraker_rc" -eq 0 ] || { log "FAIL: Moonraker step failed (exit $moonraker_rc)"; status=1; }
[ "$cancel_rc" -eq 0 ] || { log "FAIL: cancelling a running print failed (exit $cancel_rc, see out/cancel.json)"; status=1; }
[ "$status" -eq 0 ] && log "PASS"
exit "$status"
