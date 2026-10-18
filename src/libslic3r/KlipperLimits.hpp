#pragma once

#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// The motion limits of a Klipper printer as Klipper v0.13 resolves them from printer.cfg, its
// defaults included. Klipper owns these values; the slicer imports them to model Klipper's motion
// planning (KlipperMotionModel) and never emits them.
struct KlipperLimits
{
    // [printer]
    double max_velocity{ 0.0 };           // mm/s
    double max_accel{ 0.0 };              // mm/s^2
    double minimum_cruise_ratio{ 0.0 };
    double square_corner_velocity{ 0.0 }; // mm/s
    double max_z_velocity{ 0.0 };         // mm/s
    double max_z_accel{ 0.0 };            // mm/s^2
    // [extruder]
    double max_extrude_only_velocity{ 0.0 };     // mm/s
    double max_extrude_only_accel{ 0.0 };        // mm/s^2
    double instantaneous_corner_velocity{ 0.0 }; // mm/s
    double pressure_advance{ 0.0 };              // s
    double pressure_advance_smooth_time{ 0.0 };  // s
};

struct KlipperLimitsImport
{
    KlipperLimits limits;
    // Non-fatal findings, in English, one per entry: unresolved [include]s, kinematics the model
    // does not cover, live toolhead state that differs from printer.cfg.
    std::vector<std::string> warnings;
};

// Parses printer.cfg text the way Klipper reads it (sections, ':' or '=' delimiters, '#'/';'
// comments, the SAVE_CONFIG block adding the options the file above it does not set) and
// resolves the limits with Klipper's defaults. [include] files are not followed; the caller passes the concatenated text if the
// limits live in an included file. Throws Slic3r::RuntimeError when a required value is missing
// or a value is out of the range Klipper accepts.
KlipperLimitsImport parse_klipper_printer_cfg(const std::string& text);

// Parses the JSON answer of Moonraker's
//   /printer/objects/query?toolhead&configfile=settings
// (with or without the JSON-RPC "result" wrapper). The limits come from configfile.settings,
// which is printer.cfg as Klipper resolved it; toolhead is only compared against it, because its
// live values may still carry a SET_VELOCITY_LIMIT from an earlier job. Throws
// Slic3r::RuntimeError on malformed JSON or missing settings.
KlipperLimitsImport parse_moonraker_toolhead_query(const std::string& json);

// The limits a print is estimated with: the imported klipper_* snapshot when
// klipper_limits_source is not "none", otherwise derived from the profile's machine limits
// (machine_max_speed_x, machine_max_acceleration_extruding, machine_max_jerk_x,
// machine_max_speed_z, machine_max_acceleration_z, first column each) and Klipper's own defaults
// (minimum_cruise_ratio 0.5, instantaneous_corner_velocity 1, extrude-only limits derived from the
// nozzle and filament diameter, pressure advance 0 with a 0.04 s smooth time).
KlipperLimits klipper_limits_from_config(const ConfigBase& config);

// Writes an imported snapshot into a printer config, with its provenance.
void store_klipper_limits(const KlipperLimits& limits, KlipperLimitsSource source, const std::string& imported_at,
                          DynamicPrintConfig& config);

} // namespace Slic3r
