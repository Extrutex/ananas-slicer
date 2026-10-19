# Kinematics in ANANAS SLICER

Every machine profile names the Klipper `kinematics` the printer runs on. The
kinematics decide which parts move, which motors share a move, and therefore
which limits in `printer.cfg` bound a toolpath. This page shows each supported
arrangement the way Klipper models it, so a profile author can tell what a limit
in `printer.cfg` actually refers to.

All figures use the same vocabulary: light fills move, outlined parts stand
still, blue arrows are the motion under discussion, and the two-row table next
to a belt drive says which way each motor turns for a pure X or a pure Y move.

## CoreXY

![CoreXY, top view](../images/kinematics-corexy.svg)

Two stationary motors on the rear of the frame drive two continuous belt loops
that cross at the ends of the gantry. A pure X move turns both motors the same
way; a pure Y move turns them opposite ways. The only moving masses are the
gantry and the toolhead, which is why CoreXY machines carry the highest
accelerations of the supported layouts.

What this means for a profile: `max_accel` and `max_velocity` in `printer.cfg`
apply to the toolhead, not to a motor, and a diagonal move loads one motor
alone. Klipper's `[printer]` section carries one `max_accel` for XY; a profile
that sets a per-feature acceleration above it leaves the envelope.

Machines: Voron 0 / 2.4 / Trident, RatRig V-Core 3, most self-built printers.

## CoreXY AWD

![CoreXY AWD, top view](../images/kinematics-corexy-awd.svg)

The same belt path as CoreXY, but each belt is driven by two synchronised
steppers, one at each end of its side. Klipper models the second stepper of each
axis as `[stepper_x1]` and `[stepper_y1]`. The motion math is unchanged; the
available torque per belt doubles, and with it the acceleration the machine can
hold before skipping.

Machines: RatRig V-Core 4, Voron 2.4 AWD conversions.

## Cartesian (bed slinger)

![Cartesian bed slinger, front view](../images/kinematics-cartesian.svg)

One motor per axis, each driving its own rail. The X motor rides on the gantry
and moves the toolhead; the Z motors lift the whole gantry on lead screws; the Y
motor moves the bed, and with it the part. The moving mass in Y therefore grows
with the print, and a Y acceleration that is fine on an empty bed can ring a tall
part later in the job.

What this means for a profile: Klipper has one `max_accel` for XY, but on this
layout the Y axis is the weak one. A profile for a bed slinger can derate Y on
its own through `machine_max_acceleration_y`, and ANANAS SLICER keeps X and Y as
separate limits for exactly this case.

Machines: Prusa i3 family, Ender 3 and clones converted to Klipper, Sovol SV06.

## CoreXZ

![CoreXZ, front view](../images/kinematics-corexz.svg)

CoreXY turned on its side: two stationary motors share the X and Z axes through
crossed belts, and the bed moves in Y. A pure X move turns both motors the same
way; a pure Z move turns them opposite ways. Z is a belt axis here, so it moves
as fast as X and has no lead-screw backlash, but it also has no self-locking: the
gantry needs the motors energised to hold its height.

Machines: Voron Switchwire, Ender 5 Switchwire conversions.

## Delta

![Linear delta, front view](../images/kinematics-delta.svg)

Three carriages on vertical towers, each on its own belt, connected to the
effector by pairs of parallel arms. There are no X, Y or Z axes in the hardware:
Klipper solves all three carriage heights from the target point using
`delta_radius` and `arm_length`, and every move loads all three motors. For that
reason `[printer]` carries one `max_velocity` and one `max_accel` for everything,
and the printable area is a circle (`print_radius`), not a rectangle.

What this means for a profile: the bed shape is a circle, `max_z_velocity` is
usually set lower than `max_velocity` because Z moves need all three carriages to
move together, and the effector is light, so accelerations are high.

Machines: FLSun V400, T1 and S1, DeltaMaker, self-built delta machines.

## IDEX

![IDEX, top view](../images/kinematics-idex.svg)

Two toolhead carriages share one X rail, each with its own motor and belt. Klipper
drives the second head through `[dual_carriage]` and offers three modes: PRIMARY
(one head prints), COPY (both heads print the same path, offset by the carriage
distance) and MIRROR (the second head prints the mirrored path). COPY and MIRROR
halve the usable X width and need the slicer to keep the model inside that
window.

Machines: RatRig V-Core 4 IDEX, self-built dual-carriage gantries.

## Where the limits come from

| Klipper key | Section | Bounds |
|---|---|---|
| `max_velocity` | `[printer]` | toolhead speed on any axis combination |
| `max_accel` | `[printer]` | toolhead acceleration; also the ceiling for `M204` in a well-formed profile |
| `max_z_velocity`, `max_z_accel` | `[printer]` | Z moves, where Z is a separate axis |
| `square_corner_velocity` | `[printer]` | the cornering speed Klipper derives junction deviation from |
| `minimum_cruise_ratio` | `[printer]` | how much of a move must cruise before the planner smooths it |
| `delta_radius`, `arm_length`, `print_radius` | `[printer]` (delta) | geometry that replaces per-axis limits |

Klipper does not clamp `SET_VELOCITY_LIMIT` or `M204` to these values (since the
change of 2021-04-30). A slicer that emits a higher value changes the live limit.
ANANAS SLICER treats `printer.cfg` as the envelope every emitted value is checked
against; reading it live through Moonraker is on the roadmap.
