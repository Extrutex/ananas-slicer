# Virtual printer rig

An end-to-end check that G-code produced by KLIPSLICE is accepted by real,
unmodified upstream Klipper and Moonraker. It runs entirely in local Docker
containers on an internal network with no published ports: no real printer is
contacted, and nothing on the LAN can reach the rig.

```sh
tests/virtual-printer/run.sh
```

One command runs all three steps and exits non-zero if any fails. Results go
to `tests/virtual-printer/out/` (gitignored): `report.md`, the G-code, JSON
results and all logs.

| Variable | Meaning |
|---|---|
| `KLIPSLICE_BIN` | Slicer binary. Default: `<build dir>/src/Release/KLIPSLICE.app/Contents/MacOS/KLIPSLICE` on macOS, `<build dir>/package/bin/klipslice` on Linux. An extracted AppImage's `AppRun` works too. |
| `KLIPSLICE_BUILD_DIR` | Build tree the default binary is taken from, absolute or relative to the repository. Default: `build/arm64` on macOS, `build` on Linux. Ignored when `KLIPSLICE_BIN` is set. |
| `VP_KEEP_RIG=1` | Leave the Klipper and Moonraker containers running for manual inspection (`docker compose -p klipslice-vp ...`). |

Requirements: Docker with Compose v2, bash, and a KLIPSLICE build. The image
is about 450 MB and each container is limited to 512 MB of RAM. The first run
builds the image from GitHub sources (about a minute); later runs reuse it and
take about 30 seconds.

## What runs

1. **Slice.** The KLIPSLICE CLI slices `tests/data/20mm_cube.obj` with the
   shipped `Voron 2.4 350 0.4 nozzle`, `0.20mm Standard @Voron` and
   `Generic PLA @System` presets.
2. **Klipper batch mode.** `klippy.py printer.cfg -i <gcode> -o /dev/null -d <dict>`,
   upstream's own batch mode, executes every command of the file against
   `printer.cfg`: kinematics, travel limits, macros, extrusion limits,
   exclude_object. The data dictionary comes from building Klipper for its
   Linux host-MCU target inside the image.
3. **Moonraker.** A long-running Klippy with the same file-output MCU and its
   API socket open, and Moonraker connected to it. The check uploads the file
   (`/server/files/upload`), reads `/server/files/metadata`, starts the print
   (`/printer/print/start`) and follows it to the end. Because the MCU is a
   file, the print runs at CPU speed: a 9-minute cube finishes in a few seconds.

### Pass criteria

- Klipper: klippy exits 0, no G-code error, no ERROR log record, no
  `Unknown command`.
- Moonraker: Klippy ready; upload accepted; metadata has a slicer and a non-zero
  `estimated_time`; `exclude_object.objects` during the print equals the
  file's `EXCLUDE_OBJECT_DEFINE` names; `print_stats` ends `complete`; no
  error or unknown-command response in the G-code store during the print.

### Reported, not failed

- WARNING log records from Klippy.
- **Deprecated parameters**: parameters Klipper still accepts but has
  deprecated (a fixed list in `klippy_batch.py`, kept in step with the pinned
  Klipper's `docs/Config_Changes.md`). Klipper logs nothing for these.
- **Ignored parameters**: parameters given on a command line that Klipper's
  handler never read, e.g. `M73 R`. Found by observing the handlers (below),
  not from a list.
- Klipper's own motion time for the printed part, and the first-layer time,
  next to the slicer's estimates.
- Moonraker metadata fields, thumbnails, `print_stats.info`, Klipper config
  warnings. Every API response is kept in `out/moonraker.json`.

## How the parts fit

| File | Role |
|---|---|
| `Dockerfile` | One image for all three containers: pinned Klipper (host-MCU dictionary, Klippy venv, prebuilt C helper) and pinned Moonraker. Tags are checked against their commit hashes. |
| `klippy-constraints.txt`, `moonraker-constraints.txt` | Exact versions of every Python package, transitive ones included. |
| `docker-compose.yml` | `klipper-batch` (one-shot), `klipper` and `moonraker` (long-running). Internal network, no ports, containers run as the invoking user. |
| `printer.cfg` | Virtual Voron 2.4 350: CoreXY, four Z motors with quad gantry level, probe, input shaper (MZV 58.0/42.4 Hz), firmware retraction, arcs, exclude_object, virtual_sdcard, pause_resume, display_status. `PRINT_START`/`PRINT_END`/`CANCEL_PRINT` stub macros. |
| `moonraker.conf` | Loopback only, loopback is the only trusted client, no CORS, no system service provider. |
| `klippy_batch.py` | Runs `klippy.main()` in-process and observes it: log records with levels, which parameters each command handler read, toolhead print time, and the exact print time of marker commands (`--markers`). Klipper's code is not changed. |
| `check_moonraker.py` | The Moonraker API steps, run inside the moonraker container against `127.0.0.1:7125`. |
| `report.py` | Writes `out/report.md`. |
| `run.sh` | Drives everything. |
| `timing-gate.sh`, `timing_corpus.py` | The Klipper timing gate, below. |

### Why file output instead of a simulated MCU

Klippy's batch mode replaces the MCU with a file: it encodes every MCU command
from the data dictionary and writes it out, and treats the clock as already
synchronised. Everything above the MCU is upstream code doing its normal work:
config parsing, G-code dispatch, macros, the toolhead and lookahead,
kinematics, step compression, exclude_object, virtual_sdcard, the API server.
For the Moonraker step Klippy runs the same way, with input through its API
instead of a file. (With an input file Klippy disables its API server.)

What this does not exercise:

- MCU responses. Endstops and the probe report the configured trigger
  position, heaters reach their target at once (`M109`/`M190` return
  immediately), and TMC drivers cannot be configured because their register
  reads need an answering MCU, so `printer.cfg` has none.
- Real time. Prints run at CPU speed. Time-based values from the live print
  are not physical: `print_stats.print_duration` is seconds of CPU time, and
  `print_stats.filament_used` misses the extrusion after Moonraker's last
  status poll because Klipper only accumulates it when status is read.
- One error ends the batch run, by Klipper's design: in batch mode a G-code
  error exits Klippy, so only the first error of a file is reported.

A simulated AVR (simulavr) or a Linux host-MCU process would answer MCU
queries, but simulavr's AVR targets lack the pins for seven steppers plus
heaters and fans, the host-MCU process needs real GPIO character devices, and
both run in real time. For a check of the G-code, the file-output MCU is the
right level.

## Klipper timing gate

```sh
tests/virtual-printer/timing-gate.sh [--update]
```

Checks the slicer's Klipper time model (`time_model = klipper_v013`,
`KlipperMotionModel`) against Klipper itself. The slicer's estimate is a model
of Klipper's motion planning, not firmware, so it is measured against the
pinned upstream Klipper on every change:

1. Slices a small corpus (cube at 0.20 and 0.12 mm, the cube with the legacy
   `ACCEL_TO_DECEL`/`SQUARE_CORNER_VELOCITY` lines, `extruder_idler`,
   `frog_legs`) with the Voron 2.4 350 presets and reduces each slice to the
   commands Klipper executes (`timing_corpus.py normalize`): comments and blank
   lines go, some files are cut after their first layers, and a move to where
   `PRINT_START` leaves the toolhead is inserted after it, because the slicer
   cannot see into the macro.
2. Runs each file through Klipper batch mode and records the exact print time
   at every `SET_PRINT_STATS_INFO`, `EXCLUDE_OBJECT_START`/`_END` and
   `PRINT_END`: a timing callback on the last queued move fires when Klipper
   commits it, so the time is not the lagging `print_time` at dispatch. The
   first file runs twice and both runs must agree to 0.1 s.
3. Writes `reference.json` and runs the `[KlipperTimingGate]` test of
   `fff_print_tests` on the fresh corpus (`KLIPPER_TIMING_DIR`). It fails when
   the estimate differs from Klipper by more than 1.5 % over the print (layer 1
   to `PRINT_END`) or 2 % over the first layer.

`--update` copies the corpus and `reference.json` to
`tests/data/klipper_timing`, where the same test runs without Docker as part
of the normal unit tests. Run it after a Klipper bump, a change of
`printer.cfg`, or a slicer change that should show up in the corpus. The
estimate is imported from `printer.cfg` with the slicer's own printer.cfg
import (`parse_klipper_printer_cfg`), so the gate also covers that import.

Not covered: anything that needs a real printer (`PRINT_START` duration,
heating, probing), firmware retraction (`G10`/`G11` are timed with the
slicer's retraction values), arcs (Klipper splits them at `[gcode_arcs]
resolution`, the slicer at its own), and kinematics other than cartesian and
corexy.

## Updating the pinned versions

1. Set `KLIPPER_TAG`/`KLIPPER_COMMIT` or `MOONRAKER_TAG`/`MOONRAKER_COMMIT` in
   the `Dockerfile` (`git ls-remote --tags <repo>` shows both; for an annotated
   tag, take the `^{}` commit).
2. Regenerate the constraints file from a fresh build of the new requirements,
   e.g. remove the `-c` option once, build, and write
   `docker run --rm klipslice-vp:local /opt/<klippy|moonraker>-env/bin/pip freeze`
   back into the file.
3. Review `DEPRECATED_PARAMS` in `klippy_batch.py` against the new Klipper's
   `docs/Config_Changes.md`.
4. Compare the new `klippy/toolhead.py` and `klippy/kinematics/extruder.py`
   with `src/libslic3r/GCode/KlipperMotionModel.cpp`, then run
   `timing-gate.sh --update`.

## CI

`.github/workflows/virtual_printer.yml` runs the rig on `ubuntu-24.04` after
every successful "Build all" run triggered by a push to `dev`, and on demand
(`workflow_dispatch`, optionally with the ID of a "Build all" run). It builds
nothing heavy: it downloads that run's Linux x86_64 AppImage artifact, extracts
it, and runs `run.sh` with `KLIPSLICE_BIN` pointing at its `AppRun`. The report
is added to the job summary and `out/` is uploaded as an artifact.
`workflow_run` triggers only fire for workflow files on the default branch.
