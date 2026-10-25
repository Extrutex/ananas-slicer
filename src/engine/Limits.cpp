#include "Limits.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/KlipperLimits.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r::engine {

namespace {

struct ScalarRule
{
    const char* key;
    const char* unit;
};

// Print speeds are compared with the printer's max_velocity, accelerations with max_accel. Klipper does
// not clamp these: a higher request is capped silently at the toolhead, which is what the cockpit must show.
constexpr std::array<ScalarRule, 11> SPEED_RULES{ {
    { "outer_wall_speed", "mm/s" },
    { "inner_wall_speed", "mm/s" },
    { "sparse_infill_speed", "mm/s" },
    { "internal_solid_infill_speed", "mm/s" },
    { "top_surface_speed", "mm/s" },
    { "bridge_speed", "mm/s" },
    { "initial_layer_speed", "mm/s" },
    { "gap_infill_speed", "mm/s" },
    { "small_perimeter_speed", "mm/s" },
    { "support_speed", "mm/s" },
    { "travel_speed", "mm/s" },
} };

constexpr std::array<ScalarRule, 6> ACCEL_RULES{ {
    { "default_acceleration", u8"mm/s²" },
    { "outer_wall_acceleration", u8"mm/s²" },
    { "inner_wall_acceleration", u8"mm/s²" },
    { "top_surface_acceleration", u8"mm/s²" },
    { "initial_layer_acceleration", u8"mm/s²" },
    { "bridge_acceleration", u8"mm/s²" },
} };

std::string source_of(const ConfigBase& config)
{
    const auto* source = config.option<ConfigOptionEnum<KlipperLimitsSource>>("klipper_limits_source");
    if (source == nullptr)
        return "profile";
    switch (source->value) {
    case KlipperLimitsSource::PrinterCfg: return "printer.cfg";
    case KlipperLimitsSource::Moonraker:  return "moonraker";
    default:                              return "profile";
    }
}

std::string label_of(const std::string& key)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    return def != nullptr ? def->label : key;
}

// The value of a scalar float option, or NaN if the config does not carry it. A vector option reads its
// first element, the one the printer's first extruder uses.
double scalar(const ConfigBase& config, const std::string& key)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr)
        return std::nan("");
    if (const auto* v = dynamic_cast<const ConfigOptionFloat*>(option))
        return v->value;
    if (const auto* v = dynamic_cast<const ConfigOptionInt*>(option))
        return v->value;
    if (const auto* v = dynamic_cast<const ConfigOptionFloats*>(option))
        return v->values.empty() ? std::nan("") : v->values.front();
    if (const auto* v = dynamic_cast<const ConfigOptionFloatsNullable*>(option))
        return v->values.empty() ? std::nan("") : v->values.front();
    return std::nan("");
}

void add_if_over(std::vector<LimitViolation>& out, const ConfigBase& config, const std::string& key, const char* unit,
                 const char* kind, double value, double limit, const char* limit_key, const std::string& source)
{
    if (std::isnan(value) || std::isnan(limit) || limit <= 0.0)
        return;
    const char* state = value > limit ? "exceeded" : (value >= LIMIT_CAUTION_RATIO * limit ? "caution" : nullptr);
    if (state == nullptr)
        return;
    LimitViolation violation;
    violation.key         = key;
    violation.label       = label_of(key);
    violation.unit        = unit;
    violation.kind        = kind;
    violation.state       = state;
    violation.limit_key   = limit_key;
    violation.source      = source;
    violation.value       = value;
    violation.limit       = limit;
    out.push_back(std::move(violation));
}

} // namespace

std::optional<EffectiveLimits> effective_limits(const DynamicPrintConfig& config)
{
    try {
        return EffectiveLimits{ source_of(config), klipper_limits_from_config(config) };
    } catch (const RuntimeError&) {
        // A profile that states no machine limits and carries no imported snapshot has nothing to compare with.
        return std::nullopt;
    }
}

std::vector<LimitViolation> check_limits(const DynamicPrintConfig& config)
{
    std::vector<LimitViolation> out;
    const std::optional<EffectiveLimits> effective = effective_limits(config);
    if (!effective)
        return out;
    const KlipperLimits& limits = effective->limits;
    const std::string&   source = effective->source;

    for (const ScalarRule& rule : SPEED_RULES)
        add_if_over(out, config, rule.key, rule.unit, "machine", scalar(config, rule.key), limits.max_velocity,
                    "printer.max_velocity", source);
    for (const ScalarRule& rule : ACCEL_RULES)
        add_if_over(out, config, rule.key, rule.unit, "machine", scalar(config, rule.key), limits.max_accel,
                    "printer.max_accel", source);

    return out;
}

} // namespace Slic3r::engine
