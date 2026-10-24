#include "Engine.hpp"

#include "Limits.hpp"
#include "Moonraker.hpp"
#include "Schema.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/KlipperLimits.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <vector>

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r::engine {

namespace {

constexpr int PROTOCOL_VERSION = 1;

const char* const METHODS[] = { "engine.info",   "presets.list", "config.schema", "config.resolve", "limits.import",
                                "limits.check",  "model.load",   "model.bounds",  "preview.load",   "preview.chunk",
                                "moonraker.connect", "moonraker.upload", "moonraker.start", "moonraker.status", "moonraker.cancel",
                                "slice" };

const json& param(const json& params, const char* name)
{
    static const json null_value;
    return params.is_object() && params.contains(name) ? params.at(name) : null_value;
}

std::string string_param(const json& params, const char* name, bool required)
{
    const json& value = param(params, name);
    if (value.is_null()) {
        if (required)
            throw EngineError(ErrorCode::InvalidParams, std::string("missing parameter \"") + name + "\"");
        return {};
    }
    if (!value.is_string())
        throw EngineError(ErrorCode::InvalidParams, std::string("parameter \"") + name + "\" must be a string");
    return value.get<std::string>();
}

size_t index_param(const json& params, const char* name, bool required)
{
    const json& value = param(params, name);
    if (value.is_null()) {
        if (required)
            throw EngineError(ErrorCode::InvalidParams, std::string("missing parameter \"") + name + "\"");
        return 0;
    }
    if (!value.is_number_unsigned())
        throw EngineError(ErrorCode::InvalidParams, std::string("parameter \"") + name + "\" must be a non-negative integer");
    return value.get<size_t>();
}

// The transform for object `index` from the "transforms" array; an object without an entry gets the default one.
ObjectTransform transform_at(const json& transforms, size_t index)
{
    if (!transforms.is_array())
        throw EngineError(ErrorCode::InvalidParams, "\"transforms\" must be an array");
    return index < transforms.size() ? parse_transform(transforms[index]) : ObjectTransform{};
}

Vec2d printable_area_center(const DynamicPrintConfig& config)
{
    const auto* area = config.option<ConfigOptionPoints>("printable_area");
    if (area == nullptr || area->values.empty())
        return Vec2d(100., 100.);
    Vec2d lo = area->values.front(), hi = area->values.front();
    for (const Vec2d& p : area->values) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    return 0.5 * (lo + hi);
}

// The config deserializers read "bad" as 0 and ignore a value outside the definition's bounds, but the
// engine must refuse an input it cannot apply as typed (docs/design/BRAND.md 5.2: an invalid draft is
// rejected, never replaced). Returns an empty string when `text` is a valid value for `def`, otherwise why not.
std::string invalid_reason(const ConfigOptionDef& def, const std::string& text)
{
    const auto split = [](const std::string& value) {
        std::vector<std::string> tokens;
        size_t                   start = 0;
        for (size_t i = 0; i <= value.size(); ++i)
            if (i == value.size() || value[i] == ',') {
                std::string token = value.substr(start, i - start);
                token.erase(0, token.find_first_not_of(" \t"));
                token.erase(token.find_last_not_of(" \t") + 1);
                tokens.push_back(std::move(token));
                start = i + 1;
            }
        return tokens;
    };

    bool numeric = false, integral = false, percent_allowed = false;
    switch (def.type) {
    case coFloat: case coFloats:                           numeric = true; break;
    case coInt: case coInts:                               numeric = true; integral = true; break;
    case coPercent: case coPercents:                       numeric = true; percent_allowed = true; break;
    case coFloatOrPercent: case coFloatsOrPercents:        numeric = true; percent_allowed = true; break;
    default: break;
    }

    if (numeric) {
        for (const std::string& token : split(text)) {
            if (def.nullable && token == "nil")
                continue;
            if (token.empty())
                return "empty value";
            std::string number = token;
            if (percent_allowed && number.back() == '%')
                number.pop_back();
            char*        end   = nullptr;
            const double value = std::strtod(number.c_str(), &end);
            if (number.empty() || end != number.c_str() + number.size() || !std::isfinite(value))
                return "\"" + token + "\" is not a number";
            if (integral && value != std::floor(value))
                return "\"" + token + "\" is not a whole number";
            if (value < def.min || value > def.max)
                return "\"" + token + "\" is outside the allowed range";
        }
        return {};
    }
    if (def.type == coBool || def.type == coBools) {
        for (const std::string& token : split(text))
            if (token != "0" && token != "1" && !(def.nullable && token == "nil"))
                return "\"" + token + "\" is not 0 or 1";
        return {};
    }
    if ((def.type == coEnum || def.type == coEnums) && !def.enum_values.empty()) {
        for (const std::string& token : split(text))
            if (std::find(def.enum_values.begin(), def.enum_values.end(), token) == def.enum_values.end())
                return "\"" + token + "\" is not one of the allowed values";
        return {};
    }
    return {};
}

json violation_json(const LimitViolation& v)
{
    return { { "key", v.key },
             { "label", v.label },
             { "unit", v.unit },
             { "kind", v.kind },
             { "state", v.state },
             { "value", v.value },
             { "limit", v.limit },
             { "limit_key", v.limit_key },
             { "source", v.source } };
}

json limits_to_json(const KlipperLimits& l);

// The limits the violations are measured against, or null when the profile states none.
json effective_limits_json(const DynamicPrintConfig& config)
{
    const std::optional<EffectiveLimits> effective = effective_limits(config);
    if (!effective)
        return nullptr;
    return { { "source", effective->source }, { "values", limits_to_json(effective->limits) } };
}

json violations_json(const DynamicPrintConfig& config)
{
    json out = json::array();
    for (const LimitViolation& v : check_limits(config))
        out.push_back(violation_json(v));
    return out;
}

KlipperLimits limits_from_json(const json& limits)
{
    if (!limits.is_object())
        throw EngineError(ErrorCode::InvalidParams, "\"limits\" must be an object");
    KlipperLimits out;
    const auto    take = [&limits](const char* key, double& target) {
        if (!limits.contains(key))
            return;
        if (!limits.at(key).is_number())
            throw EngineError(ErrorCode::InvalidParams, std::string("limits.") + key + " must be a number");
        target = limits.at(key).get<double>();
    };
    take("max_velocity", out.max_velocity);
    take("max_accel", out.max_accel);
    take("minimum_cruise_ratio", out.minimum_cruise_ratio);
    take("square_corner_velocity", out.square_corner_velocity);
    take("max_z_velocity", out.max_z_velocity);
    take("max_z_accel", out.max_z_accel);
    take("max_extrude_only_velocity", out.max_extrude_only_velocity);
    take("max_extrude_only_accel", out.max_extrude_only_accel);
    take("instantaneous_corner_velocity", out.instantaneous_corner_velocity);
    take("pressure_advance", out.pressure_advance);
    take("pressure_advance_smooth_time", out.pressure_advance_smooth_time);
    return out;
}

json limits_to_json(const KlipperLimits& l)
{
    return { { "max_velocity", l.max_velocity },
             { "max_accel", l.max_accel },
             { "minimum_cruise_ratio", l.minimum_cruise_ratio },
             { "square_corner_velocity", l.square_corner_velocity },
             { "max_z_velocity", l.max_z_velocity },
             { "max_z_accel", l.max_z_accel },
             { "max_extrude_only_velocity", l.max_extrude_only_velocity },
             { "max_extrude_only_accel", l.max_extrude_only_accel },
             { "instantaneous_corner_velocity", l.instantaneous_corner_velocity },
             { "pressure_advance", l.pressure_advance },
             { "pressure_advance_smooth_time", l.pressure_advance_smooth_time } };
}

} // namespace

Engine::Engine(const std::string& profiles_dir, const std::string& vendor)
{
    if (!fs::is_directory(profiles_dir))
        throw EngineError(ErrorCode::EngineError, "profiles folder not found: " + profiles_dir);

    // Validation mode reads the vendors straight from the profiles folder instead of a data directory,
    // so the engine neither needs nor writes a user data tree.
    set_data_dir(profiles_dir);
    // The 3MF and STEP readers keep working copies under the temporary directory; unset, that is the
    // root of the file system and a read of any 3MF fails.
    set_temporary_dir((fs::temp_directory_path() / "ananas-engine").string());
    const fs::path resources = fs::path(profiles_dir).parent_path();
    if (fs::exists(resources / "info"))
        set_resources_dir(resources.string());

    m_bundle.set_is_validation_mode(true);
    m_bundle.set_vendor_to_validate(vendor);
    m_app_config.set("preset_folder", "default");

    std::string errors;
    m_bundle.load_presets(m_app_config, ForwardCompatibilitySubstitutionRule::Disable, PresetBundle::PresetPreferences(),
                          &errors, /*read_only=*/true);
    if (!errors.empty())
        throw EngineError(ErrorCode::EngineError, "profile load failed: " + errors);

    // System printers stay hidden until a variant is enabled; enabling every instantiable one makes
    // select_preset_by_name work for all of them.
    for (const Preset& printer : m_bundle.printers.get_presets()) {
        if (printer.vendor == nullptr)
            continue;
        const std::string model   = printer.config.opt_string("printer_model");
        const std::string variant = printer.config.opt_string("printer_variant");
        if (!model.empty() && !variant.empty())
            m_app_config.set_variant(printer.vendor->id, model, variant, true);
    }
    m_bundle.load_installed_printers(m_app_config);
}

json Engine::info() const
{
    return { { "name", "ananas-engine" },
             { "version", SLIC3R_VERSION },
             { "protocol", PROTOCOL_VERSION },
             { "methods", METHODS } };
}

json Engine::presets_list(const json& params)
{
    const std::string printer_name = string_param(params, "printer", false);
    json              out;

    if (printer_name.empty()) {
        json printers = json::array();
        for (const Preset& printer : m_bundle.printers.get_presets()) {
            if (printer.vendor == nullptr)
                continue;
            const std::string model   = printer.config.opt_string("printer_model");
            const std::string variant = printer.config.opt_string("printer_variant");
            if (model.empty() || variant.empty())
                continue;
            printers.push_back({ { "name", printer.name },
                                 { "vendor", printer.vendor->name },
                                 { "model", model },
                                 { "variant", variant },
                                 { "default_process", printer.config.opt_string("default_print_profile") } });
        }
        out["printers"] = std::move(printers);
        return out;
    }

    // The processes and filaments that are compatible with one printer.
    resolve(json{ { "printer", printer_name } });
    json processes = json::array(), filaments = json::array();
    for (const Preset& preset : m_bundle.prints.get_presets())
        if (!preset.is_default && preset.is_visible && preset.is_compatible)
            processes.push_back(preset.name);
    for (const Preset& preset : m_bundle.filaments.get_presets())
        if (!preset.is_default && preset.is_visible && preset.is_compatible)
            filaments.push_back(preset.name);
    out["printer"]   = printer_name;
    out["processes"] = std::move(processes);
    out["filaments"] = std::move(filaments);
    return out;
}

DynamicPrintConfig Engine::resolve(const json& params)
{
    const std::string printer_name  = string_param(params, "printer", true);
    const std::string process_name  = string_param(params, "process", false);
    const std::string filament_name = string_param(params, "filament", false);

    if (!m_bundle.printers.select_preset_by_name(printer_name, /*force=*/true) ||
        m_bundle.printers.get_selected_preset_name() != printer_name)
        throw EngineError(ErrorCode::NotFound, "printer preset not found: " + printer_name);

    // Start from the printer's own process and filament, as the app does on a printer change.
    const Preset& printer = m_bundle.printers.get_selected_preset();
    const std::string default_process = printer.config.opt_string("default_print_profile");
    if (!default_process.empty())
        m_bundle.prints.select_preset_by_name(default_process, /*force=*/true);
    if (const auto* default_filament = printer.config.option<ConfigOptionStrings>("default_filament_profile");
        default_filament != nullptr && !default_filament->values.empty())
        m_bundle.filaments.select_preset_by_name(default_filament->values.front(), /*force=*/true);
    m_bundle.filament_presets.assign(1, m_bundle.filaments.get_selected_preset_name());
    m_bundle.update_multi_material_filament_presets();
    m_bundle.update_compatible(PresetSelectCompatibleType::Always);

    if (!process_name.empty() &&
        (!m_bundle.prints.select_preset_by_name(process_name, /*force=*/true) ||
         m_bundle.prints.get_selected_preset_name() != process_name))
        throw EngineError(ErrorCode::NotFound, "process preset not found: " + process_name);
    if (!filament_name.empty()) {
        if (!m_bundle.filaments.select_preset_by_name(filament_name, /*force=*/true) ||
            m_bundle.filaments.get_selected_preset_name() != filament_name)
            throw EngineError(ErrorCode::NotFound, "filament preset not found: " + filament_name);
        // One filament per extruder slot: the chosen one fills every slot.
        for (std::string& slot : m_bundle.filament_presets)
            slot = filament_name;
    }

    if (m_bundle.prints.get_selected_preset().is_default || m_bundle.filaments.get_selected_preset().is_default)
        throw EngineError(ErrorCode::NotFound, "printer \"" + printer_name + "\" has no usable process or filament preset");

    DynamicPrintConfig config = m_bundle.full_config();

    const json& overrides = param(params, "overrides");
    if (!overrides.is_null()) {
        if (!overrides.is_object())
            throw EngineError(ErrorCode::InvalidParams, "\"overrides\" must be an object of key -> serialized value");
        for (const auto& [key, value] : overrides.items()) {
            if (!value.is_string())
                throw EngineError(ErrorCode::InvalidParams, "override \"" + key + "\" must be a string");
            const ConfigOptionDef* def = print_config_def.get(key);
            if (def == nullptr || !config.has(key))
                throw EngineError(ErrorCode::InvalidParams, "unknown setting \"" + key + "\"");
            std::string text = value.get<std::string>();
            if (const std::string reason = invalid_reason(*def, text); !reason.empty())
                throw EngineError(ErrorCode::InvalidParams, "cannot set \"" + key + "\": " + reason);
            // One value for a per-extruder setting applies to every extruder; assigning it as is would
            // shorten the vector and leave the other extruders without a value.
            if (!def->is_scalar() && text.find(',') == std::string::npos) {
                const auto* current = static_cast<const ConfigOptionVectorBase*>(config.option(key));
                if (current != nullptr && current->size() > 1) {
                    const std::string single = text;
                    for (size_t i = 1; i < current->size(); ++i)
                        text += "," + single;
                }
            }
            ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
            if (!config.set_deserialize_nothrow(key, text, substitutions))
                throw EngineError(ErrorCode::InvalidParams, "cannot set \"" + key + "\" to \"" + text + "\"");
        }
    }

    const json& limits = param(params, "limits");
    if (!limits.is_null()) {
        const std::string source = limits.is_object() && limits.contains("source") && limits.at("source").is_string() ?
                                       limits.at("source").get<std::string>() :
                                       "printer_cfg";
        if (source != "printer_cfg" && source != "moonraker")
            throw EngineError(ErrorCode::InvalidParams, "limits.source must be \"printer_cfg\" or \"moonraker\"");
        try {
            store_klipper_limits(limits_from_json(limits),
                                 source == "moonraker" ? KlipperLimitsSource::Moonraker : KlipperLimitsSource::PrinterCfg,
                                 "engine", config);
        } catch (const RuntimeError& ex) {
            // The values Klipper itself would refuse (a zero max_accel, a smooth time above 0.2 s, ...).
            throw EngineError(ErrorCode::InvalidParams, std::string("invalid limits: ") + ex.what());
        }
    }
    return config;
}

json Engine::config_resolve(const json& params)
{
    const DynamicPrintConfig config = resolve(params);
    json                     values = json::object();
    for (const std::string& key : config.keys())
        values[key] = config.opt_serialize(key);
    return { { "printer", m_bundle.printers.get_selected_preset_name() },
             { "process", m_bundle.prints.get_selected_preset_name() },
             { "filament", m_bundle.filaments.get_selected_preset_name() },
             { "config", std::move(values) },
             { "limits", effective_limits_json(config) },
             { "violations", violations_json(config) } };
}

json Engine::limits_import(const json& params) const
{
    const std::string printer_cfg = string_param(params, "printer_cfg", false);
    const std::string moonraker   = string_param(params, "moonraker", false);
    if (printer_cfg.empty() == moonraker.empty())
        throw EngineError(ErrorCode::InvalidParams, "give exactly one of \"printer_cfg\" (text) or \"moonraker\" (JSON text)");
    try {
        const KlipperLimitsImport imported = printer_cfg.empty() ? parse_moonraker_toolhead_query(moonraker) :
                                                                   parse_klipper_printer_cfg(printer_cfg);
        return { { "source", printer_cfg.empty() ? "moonraker" : "printer_cfg" },
                 { "limits", limits_to_json(imported.limits) },
                 { "warnings", imported.warnings } };
    } catch (const std::exception& ex) {
        throw EngineError(ErrorCode::EngineError, ex.what());
    }
}

json Engine::limits_check(const json& params)
{
    const DynamicPrintConfig config = resolve(params);
    return { { "limits", effective_limits_json(config) }, { "violations", violations_json(config) } };
}

json Engine::model_load(const json& params)
{
    const std::string path = string_param(params, "path", true);
    if (!fs::is_regular_file(path))
        throw EngineError(ErrorCode::InvalidParams, "model file not found: " + path);
    try {
        const Model model   = m_models.copy_of(path);
        json        objects = json::array();
        for (const ModelObject* object : model.objects)
            objects.push_back(object_mesh_json(*object));
        return { { "path", path }, { "objects", std::move(objects) } };
    } catch (const EngineError&) {
        throw;
    } catch (const std::exception& ex) {
        throw EngineError(ErrorCode::EngineError, std::string("cannot read the model: ") + ex.what());
    }
}

json Engine::model_bounds(const json& params)
{
    const std::string path = string_param(params, "path", true);
    if (!fs::is_regular_file(path))
        throw EngineError(ErrorCode::InvalidParams, "model file not found: " + path);
    try {
        Model model = m_models.copy_of(path);
        json  boxes = json::array();
        for (size_t i = 0; i < model.objects.size(); ++i) {
            ModelObject&        object = *model.objects[i];
            const BoundingBoxf3 box    = apply_transform(object, transform_at(param(params, "transforms"), i), Vec2d::Zero());
            const Vec3d         offset = object.instances.front()->get_offset();
            // The offset is what the slicer moves the object by, so a front end can draw it exactly there.
            boxes.push_back({ { "bbox", bounding_box_json(box) }, { "offset", { offset.x(), offset.y(), offset.z() } } });
        }
        return { { "objects", std::move(boxes) } };
    } catch (const EngineError&) {
        throw;
    } catch (const std::exception& ex) {
        throw EngineError(ErrorCode::EngineError, std::string("cannot read the model: ") + ex.what());
    }
}

json Engine::preview_load(const json& params)
{
    return m_preview.load(string_param(params, "gcode", true));
}

json Engine::preview_chunk(const json& params)
{
    const json& travel = param(params, "travel");
    return m_preview.chunk(string_param(params, "gcode", true), index_param(params, "layer_from", true), index_param(params, "layer_to", true),
                           travel.is_boolean() && travel.get<bool>());
}

namespace {

// The name a G-code is stored under on the printer: a plain file name, never a path.
std::string remote_name_of(const json& params, const std::string& fallback_path)
{
    std::string name = string_param(params, "name", false);
    if (name.empty())
        name = fs::path(fallback_path).filename().string();
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20; }))
        throw EngineError(ErrorCode::InvalidParams, "\"name\" must be a plain file name");
    return name;
}

} // namespace

json Engine::moonraker_upload(const json& params) const
{
    const MoonrakerTarget target = parse_target(params);
    const std::string     gcode  = string_param(params, "gcode", true);
    const std::string     name   = remote_name_of(params, gcode);
    const json&           print  = param(params, "print");
    const bool            start  = print.is_boolean() && print.get<bool>();
    const json            answer = Moonraker(target).upload(gcode, name, start);
    const json            item   = answer.contains("item") ? answer.at("item") : json::object();
    return { { "name", item.value("path", name) },
             { "host", target.base_url },
             { "print_started", answer.value("print_started", false) } };
}

json Engine::moonraker_start(const json& params) const
{
    const MoonrakerTarget target = parse_target(params);
    const std::string     name   = remote_name_of(params, "");
    Moonraker(target).post("/printer/print/start", { { "filename", name } });
    return { { "name", name }, { "host", target.base_url } };
}

json Engine::slice(const json& params, const json& request_id, const Notify& notify)
{
    const std::string model_path  = string_param(params, "model", true);
    const std::string output_path = string_param(params, "output", true);
    if (!fs::is_regular_file(model_path))
        throw EngineError(ErrorCode::InvalidParams, "model file not found: " + model_path);

    const DynamicPrintConfig config = resolve(params);

    try {
        Model model = m_models.copy_of(model_path);
        if (model.objects.empty())
            throw EngineError(ErrorCode::EngineError, "the model file contains no objects");
        const Vec2d center = printable_area_center(config);
        const json& transforms = param(params, "transforms");
        if (transforms.is_null()) {
            // No placement given: keep the model's own layout and put it in the middle of the bed.
            model.center_instances_around_point(center);
        } else {
            for (size_t i = 0; i < model.objects.size(); ++i)
                apply_transform(*model.objects[i], transform_at(transforms, i), center);
        }

        Print print;
        for (ModelObject* object : model.objects) {
            object->ensure_on_bed();
            print.auto_assign_extruders(object);
        }
        print.apply(model, config);

        if (const StringObjectException invalid = print.validate(); !invalid.string.empty())
            throw EngineError(ErrorCode::EngineError, "the print is not valid: " + invalid.string);

        // The core's percentages are not strictly increasing between steps; a progress bar must not go back.
        int        reported = 0;
        const auto progress = [&](int percent, const std::string& message) {
            reported = std::max(reported, std::clamp(percent, 0, 100));
            if (notify)
                notify({ { "jsonrpc", "2.0" },
                         { "method", "slice.progress" },
                         { "params", { { "id", request_id }, { "percent", reported }, { "message", message } } } });
        };
        print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
            if (status.percent >= 0)
                progress(status.percent, status.text);
        });
        print.process();

        GCodeProcessorResult result;
        const fs::path       target = fs::absolute(output_path);
        if (!target.parent_path().empty())
            fs::create_directories(target.parent_path());
        const std::string written = print.export_gcode(target.string(), &result, nullptr);
        progress(100, "Finished");

        size_t layers = 0;
        for (const PrintObject* object : print.objects())
            layers = std::max(layers, object->layer_count());

        return { { "gcode", written },
                 { "time_s", result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time },
                 { "layers", layers },
                 { "filament_mm", print.print_statistics().total_used_filament },
                 { "filament_g", print.print_statistics().total_weight },
                 { "limits", effective_limits_json(config) },
                 { "violations", violations_json(config) } };
    } catch (const EngineError&) {
        throw;
    } catch (const std::exception& ex) {
        throw EngineError(ErrorCode::EngineError, std::string("slicing failed: ") + ex.what());
    }
}

json Engine::handle(const json& request, const Notify& notify)
{
    const auto error_response = [](const json& id, ErrorCode code, const std::string& message) {
        return json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", static_cast<int>(code) }, { "message", message } } } };
    };

    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method") ||
        !request.at("method").is_string())
        return error_response(request.is_object() && request.contains("id") ? request.at("id") : json(), ErrorCode::InvalidRequest,
                              "not a JSON-RPC 2.0 request");

    const bool        has_id = request.contains("id");
    const json        id     = has_id ? request.at("id") : json();
    const std::string method = request.at("method").get<std::string>();
    const json&       params = request.contains("params") ? request.at("params") : json();

    try {
        json result;
        if (method == "engine.info")
            result = info();
        else if (method == "presets.list")
            result = presets_list(params);
        else if (method == "config.schema")
            result = { { "options", config_schema() } };
        else if (method == "config.resolve")
            result = config_resolve(params);
        else if (method == "limits.import")
            result = limits_import(params);
        else if (method == "limits.check")
            result = limits_check(params);
        else if (method == "model.load")
            result = model_load(params);
        else if (method == "model.bounds")
            result = model_bounds(params);
        else if (method == "preview.load")
            result = preview_load(params);
        else if (method == "preview.chunk")
            result = preview_chunk(params);
        else if (method == "moonraker.connect")
            result = moonraker_connect(parse_target(params));
        else if (method == "moonraker.upload")
            result = moonraker_upload(params);
        else if (method == "moonraker.start")
            result = moonraker_start(params);
        else if (method == "moonraker.status")
            result = moonraker_status(parse_target(params));
        else if (method == "moonraker.cancel") {
            const MoonrakerTarget target = parse_target(params);
            Moonraker(target).post("/printer/print/cancel");
            result = { { "host", target.base_url } };
        } else if (method == "slice")
            result = slice(params, id, notify);
        else
            throw EngineError(ErrorCode::MethodNotFound, "unknown method: " + method);
        return has_id ? json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } } : json();
    } catch (const EngineError& ex) {
        return has_id ? error_response(id, ex.code(), ex.what()) : json();
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "unexpected failure in " << method << ": " << ex.what();
        return has_id ? error_response(id, ErrorCode::EngineError, ex.what()) : json();
    }
}

} // namespace Slic3r::engine
