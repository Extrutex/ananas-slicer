; State macro check, case collision route (run.sh step 3), run in Klipper
; batch mode against state-macros/printer-case.cfg. run.sh also expects the
; restore's console warning about the two steppers in the log.

; Klipper keeps the two steppers apart.
_VP_EXPECT_PA STEPPER=Belt ADVANCE=0.06 SMOOTH_TIME=0.03
_VP_EXPECT_PA STEPPER=belt ADVANCE=0.07 SMOOTH_TIME=0.035

; A cancelled print: the extruder's own stepper gets its printer.cfg pressure
; advance back; the two ambiguous steppers keep the print's values instead of
; both getting the one configfile.settings reports.
_KLIPSLICE_SAVE
SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.2 SMOOTH_TIME=0.02
SET_PRESSURE_ADVANCE EXTRUDER=Belt ADVANCE=0.3 SMOOTH_TIME=0.01
SET_PRESSURE_ADVANCE EXTRUDER=belt ADVANCE=0.4 SMOOTH_TIME=0.015
SET_VELOCITY_LIMIT ACCEL=1500
CANCEL_PRINT
_VP_EXPECT_LIMITS VELOCITY=300 ACCEL=7000 MINIMUM_CRUISE_RATIO=0.5 SQUARE_CORNER_VELOCITY=5
_VP_EXPECT_PA EXTRUDER=extruder ADVANCE=0.04 SMOOTH_TIME=0.04
_VP_EXPECT_PA STEPPER=Belt ADVANCE=0.3 SMOOTH_TIME=0.01
_VP_EXPECT_PA STEPPER=belt ADVANCE=0.4 SMOOTH_TIME=0.015
