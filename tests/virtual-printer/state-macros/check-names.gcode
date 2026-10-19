; State macro check, stepper name route (run.sh step 3), run in Klipper batch
; mode against state-macros/printer-names.cfg. run.sh also expects the
; restore's console warning about the two steppers it skips in the log.

; Klipper knows the steppers by their names as written; quoted, the names
; survive its parameter parsing.
_VP_EXPECT_PA STEPPER="Belt'aux" ADVANCE=0.07 SMOOTH_TIME=0.035
_VP_EXPECT_PA STEPPER='Belt\aux' ADVANCE=0.08 SMOOTH_TIME=0.036
_VP_EXPECT_PA STEPPER=Tail ADVANCE=0.09 SMOOTH_TIME=0.037

; A cancelled print: the restore completes. Limits and the pressure advance of
; every other extruder stepper come back, including the one after the
; skipped names; the two skipped steppers keep the print's values.
_ANANAS_SLICER_SAVE
SET_VELOCITY_LIMIT VELOCITY=120 ACCEL=1500 MINIMUM_CRUISE_RATIO=0.2 SQUARE_CORNER_VELOCITY=9
SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.2 SMOOTH_TIME=0.02
SET_PRESSURE_ADVANCE EXTRUDER=Belt ADVANCE=0.3 SMOOTH_TIME=0.01
SET_PRESSURE_ADVANCE EXTRUDER="Belt'aux" ADVANCE=0.4 SMOOTH_TIME=0.015
SET_PRESSURE_ADVANCE EXTRUDER='Belt\aux' ADVANCE=0.45 SMOOTH_TIME=0.016
SET_PRESSURE_ADVANCE EXTRUDER=Tail ADVANCE=0.5 SMOOTH_TIME=0.017
M104 S200
CANCEL_PRINT
_VP_EXPECT_HEATERS_OFF
_VP_EXPECT_LIMITS VELOCITY=300 ACCEL=7000 MINIMUM_CRUISE_RATIO=0.5 SQUARE_CORNER_VELOCITY=5
_VP_EXPECT_PA EXTRUDER=extruder ADVANCE=0.04 SMOOTH_TIME=0.04
_VP_EXPECT_PA STEPPER=Belt ADVANCE=0.06 SMOOTH_TIME=0.03
_VP_EXPECT_PA STEPPER=Tail ADVANCE=0.09 SMOOTH_TIME=0.037
_VP_EXPECT_PA STEPPER="Belt'aux" ADVANCE=0.4 SMOOTH_TIME=0.015
_VP_EXPECT_PA STEPPER='Belt\aux' ADVANCE=0.45 SMOOTH_TIME=0.016
