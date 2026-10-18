# Klipper Motion Control Output

KlipSlice emits `SET_VELOCITY_LIMIT` for Klipper acceleration changes. When the
acceleration-to-deceleration option is enabled, it includes
`MINIMUM_CRUISE_RATIO`, replacing the removed `ACCEL_TO_DECEL` parameter. The
same conversion applies to combined acceleration/corner commands and Type1
prime-tower output.

## Ownership

Klipper's `square_corner_velocity` is tuned together with input shaping, and
`minimum_cruise_ratio` belongs to the same printer.cfg motion setup. The shipped
process profiles therefore leave both to the firmware: `default_jerk` is 0, so no
`SQUARE_CORNER_VELOCITY` is written and the per-feature jerk values are unused,
and `accel_to_decel_enable` is off, which is also the option's default. The
slicer still sets the per-feature acceleration with `SET_VELOCITY_LIMIT ACCEL=`.

A user preset or project that sets a non-zero `default_jerk` or enables
`accel_to_decel_enable` keeps sending these values; nothing is reinterpreted on
load. Calibration prints that sweep the corner velocity set it explicitly,
independent of `default_jerk`.

## State After the Print

Klipper keeps the values of `SET_VELOCITY_LIMIT` and `SET_PRESSURE_ADVANCE`
until it restarts. Without a restore, the end G-code and the next print's start
G-code run with the acceleration of the last feature, and with the slicer's
corner velocity, cruise ratio and pressure advance where the profile sends them.

With `klipper_restore_state` the G-code calls `_KLIPSLICE_SAVE` right before the
start G-code and `_KLIPSLICE_RESTORE` right before the end G-code, after the
filament end G-code. The macros ship in `resources/klipper/klipslice_state.cfg`
and are included from printer.cfg. `_KLIPSLICE_SAVE` records the toolhead's
`max_velocity`, `max_accel`, `minimum_cruise_ratio` and
`square_corner_velocity`; `_KLIPSLICE_RESTORE` sets them back and sets the
pressure advance and smooth time of every extruder stepper (the stepper of an
`[extruder]`/`[extruderN]` that has one, and every `[extruder_stepper]`) to its
printer.cfg value. The pressure advance comes from printer.cfg rather than from
the save, so a print that was cancelled before its restore does not carry its
value into the next one. It is read from `configfile.settings`, which
lower-cases section names: two extruder steppers whose names differ only in case
(`[extruder_stepper Belt]`, `[extruder_stepper belt]`) share one entry there, so
the restore leaves their pressure advance alone and says so in the console. The restore does nothing unless `_KLIPSLICE_SAVE` ran
since the last restore. `_KLIPSLICE_SAVE` in turn records nothing while a
snapshot is still unrestored: that snapshot predates a print that ended without
the restore, and the toolhead still carries that print's feature values. It
keeps the snapshot and says so in the console. A cancelled print never reaches
the end G-code, so `CANCEL_PRINT` has to call `_KLIPSLICE_RESTORE` itself:

- With Mainsail's or Fluidd's macro file (`mainsail.cfg`, `fluidd.cfg`), set
  `variable_user_cancel_macro: "_KLIPSLICE_RESTORE"` in
  `[gcode_macro _CLIENT_VARIABLE]` and do not define a `CANCEL_PRINT` of your
  own. Klipper merges sections of the same name and the later `gcode` wins, so
  a second `CANCEL_PRINT` replaces the client's, which also drops its
  `TURN_OFF_HEATERS`; `CANCEL_PRINT_BASE` (Klipper's own `CANCEL_PRINT`) leaves
  the heaters on. The client's macro runs the hook after `TURN_OFF_HEATERS` and
  `M106 S0`, right before `CANCEL_PRINT_BASE`.
- Without such a file, `CANCEL_PRINT` renames the original and runs
  `TURN_OFF_HEATERS`, `M107`, `CANCEL_PRINT_BASE` and then `_KLIPSLICE_RESTORE`
  as the last command, so a G-code error in the restore cannot keep the heaters
  on or the print running. The virtual printer (`tests/virtual-printer/printer.cfg`)
  uses exactly this macro.

`tests/virtual-printer/run.sh` checks both routes in Klipper batch mode, the
first one with Mainsail's `client.cfg` as published, and the case collision of
extruder stepper names.

The slicer cannot see printer.cfg, so it cannot tell whether the macros exist.
The option is therefore off by default and is enabled together with the
include; Klipper answers a missing macro with `Unknown command`. Firmware
retraction values sent with `SET_RETRACTION` are not restored: the command also
clears Klipper's retracted flag, which would leave the filament retracted
without Klipper knowing it.

## Profile Compatibility

The stored keys remain `accel_to_decel_enable` and `accel_to_decel_factor`, so
existing profiles and projects remain readable. The percentage converts to a
ratio using `1 - factor / 100`: 50% becomes 0.5, and 100% becomes 0.0, disabling
minimum cruising distance. This follows Klipper's documented migration from
pseudo acceleration to minimum cruise ratio. It does not change the configured
acceleration value or introduce a firmware ceiling.

Output requires firmware that supports `MINIMUM_CRUISE_RATIO`. Klipper v0.13.0
and the current pinned integration snapshot support it. Users on older firmware
without that interface must upgrade or disable `accel_to_decel_enable`; generated
acceleration changes then omit the cruise parameter. There is no automatic
firmware detection in the offline slicing pipeline. Custom start/end/tool-change
G-code is preserved and must be migrated by its author.

## Firmware Retraction

With `use_firmware_retraction`, retractions are `G10`/`G11` and Klipper's
`[firmware_retraction]` section performs the moves. By default
(`firmware_owns_retraction` on) its printer.cfg values apply and the slicer's
retraction length, speeds and extra restart length are not used. With the option
off, the slicer sends the active filament's values with `SET_RETRACTION`, so
per-filament retraction settings reach the printer. The values stay active in
Klipper after the print, as with any `SET_RETRACTION`.

`SET_RETRACTION` clears Klipper's single "retracted" flag, so a `G11` after it
is ignored and the filament stays retracted. The writer therefore mirrors that
flag and emits the command only while Klipper is not retracted: right before a
`G10`, or right after a `G11`. On a filament change the `G11` that follows
restores the previous filament's retraction with the values it was made with,
and the new values follow it. A command is only written when the values differ
from the last one sent. Custom G-code takes part in that state: a `G10`, `G11`
or `SET_RETRACTION` in it updates the mirrored flag, and its own
`SET_RETRACTION` makes the writer send the filament's values again before the
next `G10`. Macros that retract internally are not visible to the slicer.

The single flag cannot describe several independent nozzles: the writer keeps a
retraction per nozzle, so after a tool change the new nozzle's `G10` is dropped
while the parked one holds the flag, and its `G11` pushes out what the parked
nozzle retracted. Print validation therefore rejects slicer-owned values when a
print uses more than one nozzle (or more than one filament without
`single_extruder_multi_material`, or an automatic filament map on a
multi-nozzle printer), and warns about it when the values come from printer.cfg.

Klipper rejects a negative length, speeds below 1 mm/s and a negative extra
length; print validation reports those values instead of letting the command
abort the print.

## Time Estimate

For the Klipper flavor the print time comes from `KlipperMotionModel`
(`time_model = klipper_v013`, the default), a model of Klipper v0.13 motion
planning, not firmware. It is a port of what decides a move's duration in
Klipper: `toolhead.py` (`Move.calc_junction` with junction deviation from the
square corner velocity and the centripetal cap, the smoothed
`minimum_cruise_ratio` pass and lazy flushing of `LookAheadQueue`,
`SET_VELOCITY_LIMIT` and `M204` state), the extruder junction and extrude-only
limits of `kinematics/extruder.py`, and the Z limit of the cartesian and corexy
kinematics. `GCodeProcessor` keeps its own bookkeeping (moves, roles, layers,
`M73`) and hands every Klipper-flavor move to the model, which commits planned
moves in order; Klipper's synchronization points drive it: `M400` and the end of
the file drain the queue, `G4` dwells with `P` only, a change of the pressure
advance smoothing window stops the toolhead, and `G92` does not. Input shaping
and pressure advance do not change move durations in Klipper and are not
modelled. `time_model = legacy` keeps the planner shared with the other flavors,
which ignore the key.

The model needs Klipper's limits, which belong to printer.cfg. The
`klipper_*` keys hold an imported snapshot with its source
(`klipper_limits_source`: `printer_cfg` or `moonraker`) and date.
`parse_klipper_printer_cfg` reads printer.cfg the way Klipper does (the SAVE_CONFIG
block only adds options the file above it does not set, and is ignored as a whole
where Klipper finds it corrupted; `[include]` is not followed and reported) and
applies Klipper's
defaults, such as the extrude-only limits derived from the nozzle and filament
diameter; `parse_moonraker_toolhead_query` reads
`/printer/objects/query?toolhead&configfile=settings` and takes the limits from
`configfile.settings`, because the live `toolhead` values may still carry an
earlier job's `SET_VELOCITY_LIMIT` (a difference is reported). Without a snapshot
the limits come from the machine limits of the profile and Klipper's defaults.
What the slicer cannot see stays outside the model: the moves of macros such as
`PRINT_START`, heating and probing, and `[firmware_retraction]` values (`G10`/`G11`
are timed with the slicer's retraction settings).

## Verification

`tests/fff_print/test_gcodewriter.cpp` checks enabled/disabled output, percentage
boundaries, combined commands and actual Type1 prime-tower generation, and that
every state change of a print lies between the save and the restore macro. It also
replays sliced firmware-retraction output with the semantics of Klipper's
`[firmware_retraction]` on a shared nozzle, including custom G-code that
retracts or sets its own values. The
virtual fixture Dockerfile accepts an explicit repository/ref/commit triple and
rejects mismatched source hashes. FLUXRAKER's companion integration runner selects
pinned legacy/current targets and audits removed parameters before printing.
A successful slice or configuration check does not establish successful Klippy
execution; both integration targets must report an actual pass.

`tests/libslic3r/test_klipper_motion_model.cpp` checks the model against closed
forms of Klipper's planner equations (single moves, cruise ratio, corners over
angles, lengths and accelerations, extruder and Z limits), and
`test_klipper_limits.cpp` the printer.cfg and Moonraker import. The
`[KlipperTimingGate]` test in `tests/fff_print/test_gcode_timing.cpp` compares the
estimate with the print times pinned Klipper batch mode reached on a committed
corpus (`tests/data/klipper_timing`), within 1.5 % per print and 2 % on the first
layer; `tests/virtual-printer/timing-gate.sh` regenerates that corpus and its
reference.
