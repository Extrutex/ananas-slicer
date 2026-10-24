#pragma once

#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r::engine {

// One parameter that sits at or above a limit. The engine only reports; it never clamps a value
// (docs/design/BRAND.md 5.2: "Limits never clamp").
struct LimitViolation
{
    std::string key;       // slicer config key, e.g. "outer_wall_acceleration"
    std::string label;     // English label of the key as the config definition carries it
    std::string unit;      // "mm/s", "mm/s²", "mm³/s", "s"
    std::string kind;      // "machine" (printer.cfg envelope, Klipper does not enforce it) or
                           // "profile" (hardware profile). "firmware" (Klipper rejects the command) is
                           // reserved: no slicer setting can exceed a firmware limit today, because the
                           // importer already refuses the Klipper values that are out of range.
    std::string state;     // "caution" (>= 90 % of the limit) or "exceeded" (> limit)
    std::string limit_key; // where the limit comes from, e.g. "printer.max_accel"
    std::string source;    // "printer.cfg", "moonraker" or "profile" (derived from the profile's machine limits)
    double      value{ 0.0 };
    double      limit{ 0.0 };
    bool        approximate{ false }; // the compared value is computed, not read
};

// Compares the speed, acceleration and volumetric flow settings of a fully
// resolved print config with the Klipper limits that config carries (imported snapshot, or derived from
// the profile). Pure function of the config. Order: by key as listed in the rule table.
std::vector<LimitViolation> check_limits(const DynamicPrintConfig& config);

// 90 % of the limit and above is a caution (BRAND.md 5.2).
constexpr double LIMIT_CAUTION_RATIO = 0.9;

} // namespace Slic3r::engine
