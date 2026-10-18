; Print for the live cancel check (run.sh step 5), run through Moonraker
; against state-macros/printer-cancel.cfg. check_cancel.py cancels it while
; it runs the loop below.

; A print with klipper_restore_state: _KLIPSLICE_SAVE, then per-feature
; values, heaters and the part cooling fan on.
_KLIPSLICE_SAVE
SET_VELOCITY_LIMIT VELOCITY=120 ACCEL=1500 MINIMUM_CRUISE_RATIO=0.2 SQUARE_CORNER_VELOCITY=9
SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.2 SMOOTH_TIME=0.02
SET_PRESSURE_ADVANCE EXTRUDER=Belt ADVANCE=0.3 SMOOTH_TIME=0.01
M140 S60
M104 S200
M104 T1 S200
M106 S255
SET_GCODE_VARIABLE MACRO=_VP_CANCEL_STATE VARIABLE=reached VALUE=True

; The cancel point: the print stays in this loop until it is cancelled. Each
; pass advances the toolhead's print time by 1 ms, which check_cancel.py reads
; as the print's progress. (A SET_GCODE_VARIABLE counter instead grows
; Klippy's memory by tens of MB a second at the file-output MCU's speed.)
SDCARD_LOOP_BEGIN COUNT=0
G4 P1
SDCARD_LOOP_END

; Must never run: nothing after the cancel point may execute.
SET_GCODE_VARIABLE MACRO=_VP_CANCEL_STATE VARIABLE=after VALUE=True
