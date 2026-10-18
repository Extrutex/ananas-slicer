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
`parse_klipper_printer_cfg` reads printer.cfg the way Klipper does (SAVE_CONFIG
block included, `[include]` not followed and reported) and applies Klipper's
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
boundaries, combined commands and actual Type1 prime-tower generation. It also
replays sliced firmware-retraction output with the semantics of Klipper's
`[firmware_retraction]` on a shared nozzle, including custom G-code that
retracts or sets its own values. The
virtual fixture Dockerfile accepts an explicit repository/ref/commit triple and
rejects mismatched source hashes. FLUXRAKER's companion integration runner selects
pinned legacy/current targets and audits removed parameters before printing.
A successful slice or configuration check does not establish successful Klippy
execution; both integration targets must report an actual pass.
