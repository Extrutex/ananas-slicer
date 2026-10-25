#pragma once

#include "libslic3r/KlipperLimits.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r::engine {

// One parameter that sits at or above a limit. The engine only reports; it never clamps a value
// (docs/design/BRAND.md 5.2: "Limits never clamp").
struct LimitViolation
{
    std::string key;       // slicer config key, e.g. "outer_wall_acceleration"
    std::string label;     // English label of the key as the config definition carries it
    std::string unit;      // "mm/s" or "mm/s²"
    std::string kind;      // "machine": the printer.cfg envelope, which Klipper does not enforce. "profile" (a
                           // hardware profile limit) and "firmware" (Klipper rejects the command) are reserved:
                           // nothing reports them today. The slicer caps the volumetric flow at the filament's
                           // maximum itself, and the importer refuses Klipper values that are out of range.
    std::string state;     // "caution" (>= 90 % of the limit) or "exceeded" (> limit)
    std::string limit_key; // where the limit comes from, e.g. "printer.max_accel"
    std::string source;    // "printer.cfg", "moonraker" or "profile" (derived from the profile's machine limits)
    double      value{ 0.0 };
    double      limit{ 0.0 };
};

// Compares the speed and acceleration settings of a fully resolved print config with the Klipper limits that config carries (imported snapshot, or derived from
// the profile). Pure function of the config. Order: by key as listed in the rule table.
std::vector<LimitViolation> check_limits(const DynamicPrintConfig& config);

// The Klipper limits a config is checked against and where they come from: "printer.cfg", "moonraker" or
// "profile". Empty when the profile states no machine limits and carries no snapshot. A front end shows the
// limit next to every row, also when the row is nominal and produces no violation.
struct EffectiveLimits
{
    std::string   source;
    KlipperLimits limits;
};

std::optional<EffectiveLimits> effective_limits(const DynamicPrintConfig& config);

// 90 % of the limit and above is a caution (BRAND.md 5.2).
constexpr double LIMIT_CAUTION_RATIO = 0.9;

} // namespace Slic3r::engine
