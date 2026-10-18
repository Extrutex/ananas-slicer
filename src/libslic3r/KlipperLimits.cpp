#include "KlipperLimits.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>

namespace Slic3r {

namespace {

// Klipper defaults (toolhead.py, kinematics/extruder.py) for values printer.cfg may leave out.
constexpr double DEFAULT_MINIMUM_CRUISE_RATIO          = 0.5;
constexpr double DEFAULT_SQUARE_CORNER_VELOCITY        = 5.0;
constexpr double DEFAULT_INSTANTANEOUS_CORNER_VELOCITY = 1.0;
constexpr double DEFAULT_PRESSURE_ADVANCE              = 0.0;
constexpr double DEFAULT_PRESSURE_ADVANCE_SMOOTH_TIME  = 0.040;
constexpr double MAX_PRESSURE_ADVANCE_SMOOTH_TIME      = 0.200;

// Kinematics whose Z limit (check_move) the motion model reproduces.
bool is_modelled_kinematics(const std::string& kinematics) { return kinematics == "cartesian" || kinematics == "corexy"; }

using Section  = std::map<std::string, std::string>;
using Sections = std::map<std::string, Section>;

std::string trim(const std::string& s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    const auto begin     = std::find_if(s.begin(), s.end(), not_space);
    const auto end       = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Python configparser with inline_comment_prefixes=('#', ';'), as Klipper uses it: a comment
// prefix starts an inline comment only after whitespace.
std::string strip_inline_comment(const std::string& value)
{
    for (size_t i = 1; i < value.size(); ++i)
        if ((value[i] == '#' || value[i] == ';') && std::isspace(static_cast<unsigned char>(value[i - 1])))
            return value.substr(0, i);
    return value;
}

Sections parse_sections(const std::string& text, std::vector<std::string>& warnings)
{
    Sections    sections;
    Section*    current = nullptr;
    bool        in_option = false;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        // The SAVE_CONFIG block: Klipper reads the "#*# " lines as configuration overriding the
        // file above them, which is where it writes them.
        if (raw.rfind("#*#", 0) == 0) {
            raw = raw.substr(3);
            if (!raw.empty() && raw.front() == ' ')
                raw.erase(0, 1);
        }
        const std::string line = trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(raw.front()))) {
            // Continuation of a multi-line value (gcode, gantry corners); none of the limits is one.
            if (in_option)
                continue;
        }
        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos)
                throw RuntimeError("printer.cfg: malformed section header: " + line);
            const std::string name = trim(line.substr(1, close - 1));
            if (name.rfind("include ", 0) == 0)
                warnings.emplace_back("printer.cfg: [" + name + "] is not followed; pass the included file's text too if it holds [printer] or [extruder] limits");
            current   = &sections[name];
            in_option = false;
            continue;
        }
        const size_t delim = line.find_first_of(":=");
        if (delim == std::string::npos || current == nullptr) {
            in_option = false;
            continue;
        }
        const std::string key = to_lower(trim(line.substr(0, delim)));
        (*current)[key]       = trim(strip_inline_comment(line.substr(delim + 1)));
        in_option             = true;
    }
    return sections;
}

std::optional<double> parse_number(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    size_t       pos   = 0;
    const double value = string_to_double_decimal_point(text, &pos);
    if (pos != text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

class SectionReader
{
public:
    SectionReader(const Sections& sections, const std::string& name) : m_name(name)
    {
        const auto it = sections.find(name);
        if (it == sections.end())
            throw RuntimeError("Klipper config has no [" + name + "] section");
        m_section = &it->second;
    }

    bool has(const std::string& key) const { return m_section->count(key) > 0; }

    std::string get_string(const std::string& key) const
    {
        const auto it = m_section->find(key);
        if (it == m_section->end())
            throw RuntimeError("Klipper config: [" + m_name + "] " + key + " is required");
        return it->second;
    }

    // Klipper's getfloat(): `above` is exclusive, `minval`/`maxval` inclusive.
    double get_float(const std::string& key, std::optional<double> fallback, std::optional<double> above = std::nullopt,
                     std::optional<double> minval = std::nullopt, std::optional<double> maxval = std::nullopt,
                     std::optional<double> below = std::nullopt) const
    {
        double value;
        const auto it = m_section->find(key);
        if (it == m_section->end()) {
            if (!fallback)
                throw RuntimeError("Klipper config: [" + m_name + "] " + key + " is required");
            value = *fallback;
        } else {
            const std::optional<double> parsed = parse_number(it->second);
            if (!parsed)
                throw RuntimeError("Klipper config: [" + m_name + "] " + key + " is not a number: '" + it->second + "'");
            value = *parsed;
        }
        const auto fail = [&](const char* what, double bound) {
            throw RuntimeError("Klipper config: [" + m_name + "] " + key + " = " + float_to_string_decimal_point(value) +
                               " must be " + what + " " + float_to_string_decimal_point(bound));
        };
        if (above && !(value > *above))
            fail("above", *above);
        if (below && !(value < *below))
            fail("below", *below);
        if (minval && value < *minval)
            fail("at least", *minval);
        if (maxval && value > *maxval)
            fail("at most", *maxval);
        return value;
    }

private:
    std::string    m_name;
    const Section* m_section{ nullptr };
};

// Resolves the limits exactly as Klipper v0.13 reads them (toolhead.py ToolHead.__init__,
// kinematics/corexy.py and cartesian.py, kinematics/extruder.py PrinterExtruder and
// ExtruderStepper), defaults and range checks included.
KlipperLimitsImport resolve_limits(const Sections& sections, std::vector<std::string> warnings)
{
    KlipperLimitsImport result;
    result.warnings = std::move(warnings);
    KlipperLimits& limits = result.limits;

    const SectionReader printer(sections, "printer");
    const std::string kinematics = printer.get_string("kinematics");
    if (!is_modelled_kinematics(kinematics))
        result.warnings.emplace_back("kinematics '" + kinematics +
                                     "': the motion model reproduces the Z limit of cartesian and corexy only");
    limits.max_velocity = printer.get_float("max_velocity", std::nullopt, 0.);
    limits.max_accel    = printer.get_float("max_accel", std::nullopt, 0.);
    double min_cruise_ratio = DEFAULT_MINIMUM_CRUISE_RATIO;
    if (!printer.has("minimum_cruise_ratio") && printer.has("max_accel_to_decel")) {
        // Deprecated, converted as Klipper does.
        const double accel_to_decel = printer.get_float("max_accel_to_decel", std::nullopt, 0.);
        min_cruise_ratio            = 1. - std::min(1., accel_to_decel / limits.max_accel);
        result.warnings.emplace_back("[printer] max_accel_to_decel is deprecated; converted to minimum_cruise_ratio " +
                                     float_to_string_decimal_point(min_cruise_ratio));
    }
    limits.minimum_cruise_ratio   = printer.get_float("minimum_cruise_ratio", min_cruise_ratio, std::nullopt, 0., std::nullopt, 1.);
    limits.square_corner_velocity = printer.get_float("square_corner_velocity", DEFAULT_SQUARE_CORNER_VELOCITY, std::nullopt, 0.);
    limits.max_z_velocity = printer.get_float("max_z_velocity", limits.max_velocity, 0., std::nullopt, limits.max_velocity);
    limits.max_z_accel    = printer.get_float("max_z_accel", limits.max_accel, 0., std::nullopt, limits.max_accel);

    const SectionReader extruder(sections, "extruder");
    const double nozzle_diameter   = extruder.get_float("nozzle_diameter", std::nullopt, 0.);
    const double filament_diameter = extruder.get_float("filament_diameter", std::nullopt, std::nullopt, nozzle_diameter);
    const double filament_area         = M_PI * std::pow(filament_diameter * .5, 2);
    const double def_max_extrude_ratio = 4. * nozzle_diameter * nozzle_diameter / filament_area;
    limits.max_extrude_only_velocity =
        extruder.get_float("max_extrude_only_velocity", limits.max_velocity * def_max_extrude_ratio, 0.);
    limits.max_extrude_only_accel = extruder.get_float("max_extrude_only_accel", limits.max_accel * def_max_extrude_ratio, 0.);
    limits.instantaneous_corner_velocity =
        extruder.get_float("instantaneous_corner_velocity", DEFAULT_INSTANTANEOUS_CORNER_VELOCITY, std::nullopt, 0.);
    limits.pressure_advance = extruder.get_float("pressure_advance", DEFAULT_PRESSURE_ADVANCE, std::nullopt, 0.);
    limits.pressure_advance_smooth_time = extruder.get_float("pressure_advance_smooth_time", DEFAULT_PRESSURE_ADVANCE_SMOOTH_TIME,
                                                             0., std::nullopt, MAX_PRESSURE_ADVANCE_SMOOTH_TIME);
    return result;
}

void validate(const KlipperLimits& limits)
{
    const auto require = [](bool ok, const char* what) {
        if (!ok)
            throw RuntimeError(std::string("Klipper limits: ") + what);
    };
    require(limits.max_velocity > 0., "max_velocity must be above 0");
    require(limits.max_accel > 0., "max_accel must be above 0");
    require(limits.minimum_cruise_ratio >= 0. && limits.minimum_cruise_ratio < 1., "minimum_cruise_ratio must be in [0, 1)");
    require(limits.square_corner_velocity >= 0., "square_corner_velocity must not be negative");
    require(limits.max_z_velocity > 0., "max_z_velocity must be above 0");
    require(limits.max_z_accel > 0., "max_z_accel must be above 0");
    require(limits.max_extrude_only_velocity > 0., "max_extrude_only_velocity must be above 0");
    require(limits.max_extrude_only_accel > 0., "max_extrude_only_accel must be above 0");
    require(limits.instantaneous_corner_velocity >= 0., "instantaneous_corner_velocity must not be negative");
    require(limits.pressure_advance >= 0., "pressure_advance must not be negative");
    require(limits.pressure_advance_smooth_time > 0. && limits.pressure_advance_smooth_time <= MAX_PRESSURE_ADVANCE_SMOOTH_TIME,
            "pressure_advance_smooth_time must be in (0, 0.2]");
}

double first_value(const ConfigBase& config, const char* key)
{
    const ConfigOptionFloats* opt = config.option<ConfigOptionFloats>(key);
    if (opt == nullptr || opt->values.empty())
        throw RuntimeError(std::string("Klipper time model: the printer config has no ") + key);
    return opt->values.front();
}

double float_value(const ConfigBase& config, const char* key)
{
    const ConfigOptionFloat* opt = config.option<ConfigOptionFloat>(key);
    if (opt == nullptr)
        throw RuntimeError(std::string("Klipper time model: the printer config has no ") + key);
    return opt->value;
}

} // namespace

KlipperLimitsImport parse_klipper_printer_cfg(const std::string& text)
{
    std::vector<std::string> warnings;
    const Sections sections = parse_sections(text, warnings);
    return resolve_limits(sections, std::move(warnings));
}

KlipperLimitsImport parse_moonraker_toolhead_query(const std::string& json_text)
{
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception& err) {
        throw RuntimeError(std::string("Moonraker answer is not valid JSON: ") + err.what());
    }
    const nlohmann::json* status = nullptr;
    if (root.is_object() && root.contains("result") && root["result"].is_object() && root["result"].contains("status"))
        status = &root["result"]["status"];
    else if (root.is_object() && root.contains("status"))
        status = &root["status"];
    if (status == nullptr || !status->is_object())
        throw RuntimeError("Moonraker answer has no status object; query /printer/objects/query?toolhead&configfile=settings");
    if (!status->contains("configfile") || !(*status)["configfile"].is_object() ||
        !(*status)["configfile"].contains("settings") || !(*status)["configfile"]["settings"].is_object())
        throw RuntimeError("Moonraker answer has no configfile.settings; query /printer/objects/query?toolhead&configfile=settings");

    // configfile.settings is printer.cfg as Klipper parsed it: typed values, lower-case names.
    Sections sections;
    for (const auto& [section_name, options] : (*status)["configfile"]["settings"].items()) {
        if (!options.is_object())
            continue;
        Section& section = sections[section_name];
        for (const auto& [key, value] : options.items()) {
            if (value.is_number())
                section[to_lower(key)] = value.dump();
            else if (value.is_string())
                section[to_lower(key)] = value.get<std::string>();
        }
    }
    KlipperLimitsImport result = resolve_limits(sections, {});

    // The live toolhead keeps SET_VELOCITY_LIMIT changes until Klipper restarts, so it is only compared.
    if (status->contains("toolhead") && (*status)["toolhead"].is_object()) {
        const nlohmann::json& toolhead = (*status)["toolhead"];
        const std::pair<const char*, double> configured[] = {
            { "max_velocity", result.limits.max_velocity },
            { "max_accel", result.limits.max_accel },
            { "minimum_cruise_ratio", result.limits.minimum_cruise_ratio },
            { "square_corner_velocity", result.limits.square_corner_velocity },
        };
        for (const auto& [key, cfg_value] : configured) {
            if (!toolhead.contains(key) || !toolhead[key].is_number())
                continue;
            const double live = toolhead[key].get<double>();
            if (std::abs(live - cfg_value) > 1e-9 * std::max(1., std::abs(cfg_value)))
                result.warnings.emplace_back(std::string("toolhead.") + key + " is " + float_to_string_decimal_point(live) +
                                             ", printer.cfg has " + float_to_string_decimal_point(cfg_value) +
                                             "; a SET_VELOCITY_LIMIT from an earlier job is still active, the printer.cfg value is used");
        }
    }
    return result;
}

KlipperLimits klipper_limits_from_config(const ConfigBase& config)
{
    // A DynamicPrintConfig holds enums as ConfigOptionEnumGeneric, a PrintConfig as ConfigOptionEnum<T>;
    // both answer getInt(). An absent key is a config without a snapshot.
    const ConfigOption* source = config.option("klipper_limits_source");
    KlipperLimits limits;
    if (source != nullptr && static_cast<KlipperLimitsSource>(source->getInt()) != KlipperLimitsSource::None) {
        limits.max_velocity                  = float_value(config, "klipper_max_velocity");
        limits.max_accel                     = float_value(config, "klipper_max_accel");
        limits.minimum_cruise_ratio          = float_value(config, "klipper_minimum_cruise_ratio");
        limits.square_corner_velocity        = float_value(config, "klipper_square_corner_velocity");
        limits.max_z_velocity                = float_value(config, "klipper_max_z_velocity");
        limits.max_z_accel                   = float_value(config, "klipper_max_z_accel");
        limits.max_extrude_only_velocity     = float_value(config, "klipper_max_extrude_only_velocity");
        limits.max_extrude_only_accel        = float_value(config, "klipper_max_extrude_only_accel");
        limits.instantaneous_corner_velocity = float_value(config, "klipper_instantaneous_corner_velocity");
        limits.pressure_advance              = float_value(config, "klipper_pressure_advance");
        limits.pressure_advance_smooth_time  = float_value(config, "klipper_pressure_advance_smooth_time");
    } else {
        // Not imported: what the profile states about the machine, completed with Klipper's defaults.
        limits.max_velocity           = first_value(config, "machine_max_speed_x");
        limits.max_accel              = first_value(config, "machine_max_acceleration_extruding");
        limits.square_corner_velocity = first_value(config, "machine_max_jerk_x");
        limits.minimum_cruise_ratio   = DEFAULT_MINIMUM_CRUISE_RATIO;
        if (limits.max_velocity <= 0. || limits.max_accel <= 0.)
            throw RuntimeError("Klipper time model: machine_max_speed_x and machine_max_acceleration_extruding must be above 0, "
                               "or import the Klipper limits");
        // Klipper defaults Z to the XY limits and caps it there.
        const double max_z_velocity = first_value(config, "machine_max_speed_z");
        const double max_z_accel    = first_value(config, "machine_max_acceleration_z");
        limits.max_z_velocity = (max_z_velocity > 0.) ? std::min(max_z_velocity, limits.max_velocity) : limits.max_velocity;
        limits.max_z_accel    = (max_z_accel > 0.) ? std::min(max_z_accel, limits.max_accel) : limits.max_accel;
        const double nozzle_diameter   = first_value(config, "nozzle_diameter");
        const double filament_diameter = first_value(config, "filament_diameter");
        if (nozzle_diameter <= 0. || filament_diameter <= 0.)
            throw RuntimeError("Klipper time model: nozzle_diameter and filament_diameter must be above 0");
        const double def_max_extrude_ratio   = 4. * nozzle_diameter * nozzle_diameter / (M_PI * std::pow(filament_diameter * .5, 2));
        limits.max_extrude_only_velocity     = limits.max_velocity * def_max_extrude_ratio;
        limits.max_extrude_only_accel        = limits.max_accel * def_max_extrude_ratio;
        limits.instantaneous_corner_velocity = DEFAULT_INSTANTANEOUS_CORNER_VELOCITY;
        limits.pressure_advance              = DEFAULT_PRESSURE_ADVANCE;
        limits.pressure_advance_smooth_time  = DEFAULT_PRESSURE_ADVANCE_SMOOTH_TIME;
    }
    validate(limits);
    return limits;
}

void store_klipper_limits(const KlipperLimits& limits, KlipperLimitsSource source, const std::string& imported_at,
                          DynamicPrintConfig& config)
{
    if (source == KlipperLimitsSource::None)
        throw InvalidArgument("store_klipper_limits: an imported snapshot needs a source");
    validate(limits);
    config.set_key_value("klipper_limits_source", new ConfigOptionEnum<KlipperLimitsSource>(source));
    config.set_key_value("klipper_limits_imported_at", new ConfigOptionString(imported_at));
    config.set_key_value("klipper_max_velocity", new ConfigOptionFloat(limits.max_velocity));
    config.set_key_value("klipper_max_accel", new ConfigOptionFloat(limits.max_accel));
    config.set_key_value("klipper_minimum_cruise_ratio", new ConfigOptionFloat(limits.minimum_cruise_ratio));
    config.set_key_value("klipper_square_corner_velocity", new ConfigOptionFloat(limits.square_corner_velocity));
    config.set_key_value("klipper_max_z_velocity", new ConfigOptionFloat(limits.max_z_velocity));
    config.set_key_value("klipper_max_z_accel", new ConfigOptionFloat(limits.max_z_accel));
    config.set_key_value("klipper_max_extrude_only_velocity", new ConfigOptionFloat(limits.max_extrude_only_velocity));
    config.set_key_value("klipper_max_extrude_only_accel", new ConfigOptionFloat(limits.max_extrude_only_accel));
    config.set_key_value("klipper_instantaneous_corner_velocity", new ConfigOptionFloat(limits.instantaneous_corner_velocity));
    config.set_key_value("klipper_pressure_advance", new ConfigOptionFloat(limits.pressure_advance));
    config.set_key_value("klipper_pressure_advance_smooth_time", new ConfigOptionFloat(limits.pressure_advance_smooth_time));
}

} // namespace Slic3r
