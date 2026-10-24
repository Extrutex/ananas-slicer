#include "Schema.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cfloat>
#include <set>

namespace Slic3r::engine {

namespace {

const char* type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat:             return "float";
    case coFloats:            return "floats";
    case coInt:               return "int";
    case coInts:              return "ints";
    case coString:            return "string";
    case coStrings:           return "strings";
    case coPercent:           return "percent";
    case coPercents:          return "percents";
    case coFloatOrPercent:    return "float_or_percent";
    case coFloatsOrPercents:  return "floats_or_percents";
    case coPoint:             return "point";
    case coPoints:            return "points";
    case coPoint3:            return "point3";
    case coBool:              return "bool";
    case coBools:             return "bools";
    case coEnum:              return "enum";
    case coEnums:             return "enums";
    default:                  return "unknown";
    }
}

const char* mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comSimple:   return "simple";
    case comAdvanced: return "advanced";
    case comExpert:   return "expert";
    default:          return "develop";
    }
}

// Keys of a preset type, in the order the presets list them.
void add_scope(nlohmann::json& options, std::set<std::string>& seen, const std::vector<std::string>& keys, const char* scope)
{
    for (const std::string& key : keys) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr || !seen.insert(key).second)
            continue;

        nlohmann::json option = {
            { "key", key },
            { "scope", scope },
            { "type", type_name(def->type) },
            { "vector", !def->is_scalar() },
            { "label", def->label },
            { "category", def->category },
            { "tooltip", def->tooltip },
            { "unit", def->sidetext },
            { "mode", mode_name(def->mode) },
            { "readonly", def->readonly },
            { "nullable", def->nullable },
        };
        if (def->min > -FLT_MAX)
            option["min"] = def->min;
        if (def->max < FLT_MAX)
            option["max"] = def->max;
        if (!def->enum_values.empty()) {
            option["enum_values"] = def->enum_values;
            option["enum_labels"] = def->enum_labels;
        }
        if (def->default_value)
            option["default"] = def->default_value->serialize();
        options.push_back(std::move(option));
    }
}

} // namespace

nlohmann::json config_schema()
{
    nlohmann::json        options = nlohmann::json::array();
    std::set<std::string> seen;
    add_scope(options, seen, Preset::print_options(), "print");
    add_scope(options, seen, Preset::filament_options(), "filament");
    add_scope(options, seen, Preset::printer_options(), "printer");
    return options;
}

} // namespace Slic3r::engine
