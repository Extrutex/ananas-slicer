#include "MoonrakerFilaments.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

int MoonrakerFilamentState::loaded_count() const
{
    return int(std::count_if(slots.begin(), slots.end(), [](const MoonrakerFilamentSlot &s) { return s.loaded; }));
}

namespace MoonrakerFilaments {

namespace {

bool is_numeric(const std::string &value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// A decimal slot or lane number; `fallback` for anything else, including a number too long for an int.
int to_int(const std::string &value, int fallback)
{
    if (!is_numeric(value) || value.size() > 9)
        return fallback;
    return std::stoi(value);
}

// The RRGGBB prefix of a hex colour as a number; 0 when the string has no six hex digits in front.
unsigned hex_rgb(const std::string &value)
{
    if (value.size() < 6)
        return 0u;
    unsigned rgb = 0;
    for (size_t i = 0; i < 6; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!std::isxdigit(c))
            return 0u;
        rgb = rgb * 16 + unsigned(std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10);
    }
    return rgb;
}

std::string json_string(const nlohmann::json &obj, const char *key)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int json_int(const nlohmann::json &obj, const char *key)
{
    auto it = obj.find(key);
    if (it == obj.end())
        return 0;
    if (it->is_number())
        return it->get<int>();
    if (it->is_string())
        return to_int(it->get<std::string>(), 0);
    return 0;
}

std::string array_string(const nlohmann::json &arr, int idx)
{
    return arr.is_array() && idx >= 0 && idx < int(arr.size()) && arr[idx].is_string() ? arr[idx].get<std::string>() : std::string();
}

int array_int(const nlohmann::json &arr, int idx)
{
    return arr.is_array() && idx >= 0 && idx < int(arr.size()) && arr[idx].is_number() ? arr[idx].get<int>() : 0;
}

// result.status of a /printer/objects/query reply, or a null json.
const nlohmann::json &query_status(const nlohmann::json &reply)
{
    static const nlohmann::json none;
    auto result = reply.find("result");
    if (result == reply.end() || !result->is_object())
        return none;
    auto status = result->find("status");
    return status != result->end() && status->is_object() ? *status : none;
}

void sort_slots(MoonrakerFilamentState &out)
{
    std::sort(out.slots.begin(), out.slots.end(), [](const MoonrakerFilamentSlot &a, const MoonrakerFilamentSlot &b) { return a.index < b.index; });
}

// One GET with Moonraker's optional X-Api-Key. Returns false with `error` set on a transport
// failure or a non-2xx status; `status` carries the HTTP status either way.
bool http_get(const std::string &url, const DynamicPrintConfig &config, std::string &body, unsigned &status, std::string &error)
{
    bool ok = false;
    status  = 0;
    auto http = Http::get(url);
    const std::string apikey = config.has("printhost_apikey") ? config.opt_string("printhost_apikey") : std::string();
    const std::string cafile = config.has("printhost_cafile") ? config.opt_string("printhost_cafile") : std::string();
    if (!apikey.empty())
        http.header("X-Api-Key", apikey);
    if (!cafile.empty())
        http.ca_file(cafile);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string reply, unsigned http_status) {
            status = http_status;
            body   = std::move(reply);
            ok     = true;
        })
        .on_error([&](std::string reply, std::string err, unsigned http_status) {
            status = http_status;
            body   = std::move(reply);
            error  = err;
            if (http_status > 0)
                error += " (HTTP " + std::to_string(http_status) + ")";
        })
        .perform_sync();
    return ok;
}

bool http_get_json(const std::string &url, const DynamicPrintConfig &config, nlohmann::json &reply, unsigned &status, std::string &error)
{
    std::string body;
    if (!http_get(url, config, body, status, error))
        return false;
    reply = nlohmann::json::parse(body, nullptr, false, true);
    if (reply.is_discarded()) {
        error = "invalid JSON from " + url;
        return false;
    }
    return true;
}

// The INI-like dictionary a Qidi box ships: [colordict] maps colour indices to RRGGBB(AA),
// [filaN] sections carry "filament = <name>".
void parse_qidi_filament_list(const std::string &content, std::map<int, std::string> &colors, std::map<int, std::string> &filaments)
{
    std::istringstream stream(content);
    std::string        line;
    bool               in_colordict = false;
    int                fila_index   = -1;
    while (std::getline(stream, line)) {
        boost::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            in_colordict = line == "[colordict]";
            fila_index   = -1;
            if (boost::starts_with(line, "[fila") && line.back() == ']') {
                const std::string num = line.substr(5, line.size() - 6);
                fila_index            = to_int(num, -1);
            }
            continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        std::string key   = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        boost::trim(key);
        boost::trim(value);
        if (in_colordict && to_int(key, -1) >= 0)
            colors[to_int(key, -1)] = value;
        else if (fila_index > 0 && key == "filament")
            filaments[fila_index] = value;
    }
}

// Snapmaker reports the sub type (CF, GF, SnapSpeed, Silk, ...) separately from the base type.
std::string combine_snapmaker_type(const std::string &type, const std::string &sub_type)
{
    const std::string base = normalize_material(type);
    const std::string sub  = normalize_material(sub_type);
    if (base.empty())
        return "PLA";
    if (sub.empty() || sub == "NONE")
        return base;
    if (sub == "CF")
        return base + "-CF";
    if (sub == "GF")
        return base + "-GF";
    if (sub == "SNAPSPEED" || sub == "HS")
        return base + " HIGH SPEED";
    if (sub == "SILK" || sub == "WOOD" || sub == "MATTE" || sub == "MARBLE")
        return base + " " + sub;
    return base; // a brand name such as Polylite or Basic says nothing about the material
}

// First visible, compatible system base preset of exactly this filament_type.
std::string id_by_type(const PresetCollection &filaments, const std::string &type)
{
    for (const Preset &p : filaments.get_presets())
        if (p.is_system && p.is_visible && p.is_compatible && filaments.get_preset_base(p) == &p &&
            boost::iequals(p.config.opt_string("filament_type", 0u), type))
            return p.filament_id;
    return {};
}

// The vendor's own preset of this material whose default colour is closest to the slot's.
std::string id_by_vendor_and_color(const PresetCollection &filaments, const std::string &vendor, const std::string &type, const std::string &color_rrggbbaa)
{
    if (vendor.empty() || color_rrggbbaa.size() != 8)
        return {};
    const unsigned target = hex_rgb(color_rrggbbaa);
    std::string    best;
    unsigned       best_distance = ~0u;
    for (const Preset &p : filaments.get_presets()) {
        if (!p.is_visible || !p.is_compatible || filaments.get_preset_base(p) != &p)
            continue;
        if (!boost::iequals(p.config.opt_string("filament_vendor", 0u), vendor) ||
            !boost::iequals(p.config.opt_string("filament_type", 0u), type))
            continue;
        std::string preset_color = p.config.opt_string("default_filament_colour", 0u);
        if (!preset_color.empty() && preset_color[0] == '#')
            preset_color.erase(0, 1);
        const unsigned value = hex_rgb(preset_color);
        const int dr = int((target >> 16) & 0xff) - int((value >> 16) & 0xff);
        const int dg = int((target >> 8) & 0xff) - int((value >> 8) & 0xff);
        const int db = int(target & 0xff) - int(value & 0xff);
        const unsigned distance = unsigned(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best          = p.filament_id;
        }
    }
    return best;
}

} // namespace

std::string base_url(const DynamicPrintConfig &printer_config)
{
    std::string host = printer_config.has("print_host") ? printer_config.opt_string("print_host") : std::string();
    boost::trim(host);
    if (host.empty())
        return {};
    const std::string port = printer_config.has("printhost_port") ? printer_config.opt_string("printhost_port") : std::string();
    const bool has_scheme = boost::istarts_with(host, "http://") || boost::istarts_with(host, "https://");
    const std::string authority = has_scheme ? host.substr(host.find("://") + 3) : host;
    if (is_numeric(port) && authority.find(':') == std::string::npos && authority.find('/') == std::string::npos)
        host += ":" + port;
    if (!has_scheme)
        host = "http://" + host;
    while (host.size() > 1 && host.back() == '/')
        host.pop_back();
    return host;
}

std::string normalize_color(const std::string &color)
{
    std::string value = color;
    boost::trim(value);
    if (boost::istarts_with(value, "0x"))
        value.erase(0, 2);
    if (!value.empty() && value[0] == '#')
        value.erase(0, 1);
    std::string hex;
    for (char c : value)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    if (hex.size() == 6)
        hex += "FF";
    return hex.size() == 8 ? hex : "00000000";
}

std::string normalize_material(const std::string &material, bool collapse_to_family)
{
    std::string value = boost::to_upper_copy(material);
    boost::trim(value);
    if (!collapse_to_family)
        return value;
    for (const char *family : {"PLA", "PETG", "ABS", "ASA", "TPU", "PVA", "PC"})
        if (value.find(family) != std::string::npos)
            return family;
    if (value.find("PA") != std::string::npos || value.find("NYLON") != std::string::npos)
        return "PA";
    return value;
}

bool parse_lane_data(const nlohmann::json &reply, MoonrakerFilamentState &out)
{
    // { "result": { "namespace": "lane_data", "value": { "lane1": { "lane": "0", "material": "PLA", "color": "#ff0000", ... } } } }
    auto result = reply.find("result");
    if (result == reply.end() || !result->is_object())
        return false;
    auto value = result->find("value");
    if (value == result->end() || !value->is_object() || value->empty())
        return false;
    out = MoonrakerFilamentState{};
    for (const auto &[key, lane] : value->items()) {
        if (!lane.is_object())
            continue;
        const std::string lane_str = json_string(lane, "lane");
        int index = to_int(lane_str, -1);
        if (index < 0 && lane.contains("lane") && lane["lane"].is_number())
            index = lane["lane"].get<int>();
        if (index < 0)
            continue;
        MoonrakerFilamentSlot slot;
        slot.index       = index;
        slot.material    = normalize_material(json_string(lane, "material"));
        slot.color       = normalize_color(json_string(lane, "color"));
        slot.bed_temp    = json_int(lane, "bed_temp");
        slot.nozzle_temp = json_int(lane, "nozzle_temp");
        slot.loaded      = !slot.material.empty();
        out.slots.push_back(slot);
    }
    if (out.slots.empty())
        return false;
    sort_slots(out);
    out.source = MoonrakerFilamentState::Source::LaneData;
    return true;
}

bool parse_happy_hare(const nlohmann::json &reply, MoonrakerFilamentState &out)
{
    // { "result": { "status": { "mmu": { "num_gates": 8, "gate_status": [...], "gate_material": [...], "gate_color": [...], "gate_temperature": [...] } } } }
    const nlohmann::json &status = query_status(reply);
    auto mmu = status.find("mmu");
    if (mmu == status.end() || !mmu->is_object() || mmu->empty())
        return false;
    const int num_gates = json_int(*mmu, "num_gates");
    if (num_gates <= 0)
        return false;
    const nlohmann::json empty = nlohmann::json::array();
    const nlohmann::json &gate_status      = mmu->contains("gate_status") ? (*mmu)["gate_status"] : empty;
    const nlohmann::json &gate_material    = mmu->contains("gate_material") ? (*mmu)["gate_material"] : empty;
    const nlohmann::json &gate_color       = mmu->contains("gate_color") ? (*mmu)["gate_color"] : empty;
    const nlohmann::json &gate_temperature = mmu->contains("gate_temperature") ? (*mmu)["gate_temperature"] : empty;
    out = MoonrakerFilamentState{};
    for (int gate = 0; gate < num_gates; ++gate) {
        MoonrakerFilamentSlot slot;
        slot.index       = gate;
        slot.material    = normalize_material(array_string(gate_material, gate));
        slot.color       = normalize_color(array_string(gate_color, gate));
        slot.nozzle_temp = array_int(gate_temperature, gate);
        // gate_status: -1 unknown, 0 empty, 1 available, 2 available from buffer
        slot.loaded = array_int(gate_status, gate) > 0 && !slot.material.empty();
        out.slots.push_back(slot);
    }
    out.source = MoonrakerFilamentState::Source::HappyHare;
    return true;
}

bool parse_qidi_box(const nlohmann::json &reply, const std::string &filament_list_cfg, MoonrakerFilamentState &out)
{
    // result.status.save_variables.variables holds box_count and color_slotN / filament_slotN / vendor_slotN,
    // result.status["box_stepper slotN"].runout_button is 0 while filament is present.
    const nlohmann::json &status = query_status(reply);
    auto save_variables = status.find("save_variables");
    if (save_variables == status.end() || !save_variables->is_object())
        return false;
    auto variables = save_variables->find("variables");
    if (variables == save_variables->end() || !variables->is_object() || !variables->contains("box_count"))
        return false;
    std::map<int, std::string> colors, filaments;
    parse_qidi_filament_list(filament_list_cfg, colors, filaments);
    const int box_count = std::max(0, variables->value("box_count", 0));
    out = MoonrakerFilamentState{};
    for (int i = 0; i < box_count * 4; ++i) {
        MoonrakerFilamentSlot slot;
        slot.index = i;
        const std::string stepper_key = "box_stepper slot" + std::to_string(i);
        auto stepper = status.find(stepper_key);
        if (stepper != status.end() && stepper->is_object()) {
            auto runout = stepper->find("runout_button");
            if (runout != stepper->end() && runout->is_number())
                slot.loaded = runout->get<int>() == 0;
        }
        if (slot.loaded) {
            const int filament_index = variables->value("filament_slot" + std::to_string(i), 1);
            const int color_index    = variables->value("color_slot" + std::to_string(i), 1);
            auto      name           = filaments.find(filament_index);
            slot.material            = normalize_material(name != filaments.end() ? name->second : "PLA", true);
            auto color               = colors.find(color_index);
            slot.color               = normalize_color(color != colors.end() ? color->second : "FFFFFFFF");
        }
        out.slots.push_back(slot);
    }
    out.source = MoonrakerFilamentState::Source::QidiBox;
    return true;
}

bool parse_snapmaker(const nlohmann::json &reply, MoonrakerFilamentState &out)
{
    // result.status.print_task_config: filament_exist[], filament_type[], filament_sub_type[], filament_color_rgba[], filament_vendor[]
    // result.status.filament_detect.info[]: { "VENDOR", "BED_TEMP", "FIRST_LAYER_TEMP" } from the NFC tag
    const nlohmann::json &status = query_status(reply);
    auto ptc = status.find("print_task_config");
    if (ptc == status.end() || !ptc->is_object())
        return false;
    auto exist = ptc->find("filament_exist");
    if (exist == ptc->end() || !exist->is_array() || exist->empty())
        return false;
    const nlohmann::json empty = nlohmann::json::array();
    const nlohmann::json &types   = ptc->contains("filament_type") ? (*ptc)["filament_type"] : empty;
    const nlohmann::json &subs    = ptc->contains("filament_sub_type") ? (*ptc)["filament_sub_type"] : empty;
    const nlohmann::json &colors  = ptc->contains("filament_color_rgba") ? (*ptc)["filament_color_rgba"] : empty;
    const nlohmann::json &vendors = ptc->contains("filament_vendor") ? (*ptc)["filament_vendor"] : empty;
    nlohmann::json nfc = empty;
    if (auto detect = status.find("filament_detect"); detect != status.end() && detect->is_object() && detect->contains("info"))
        nfc = (*detect)["info"];
    out = MoonrakerFilamentState{};
    for (int i = 0; i < int(exist->size()); ++i) {
        MoonrakerFilamentSlot slot;
        slot.index  = i;
        slot.loaded = (*exist)[i].is_boolean() && (*exist)[i].get<bool>();
        if (slot.loaded) {
            slot.material = combine_snapmaker_type(array_string(types, i), array_string(subs, i));
            slot.color    = normalize_color(colors.is_array() && i < int(colors.size()) && colors[i].is_string() ? colors[i].get<std::string>() : "FFFFFFFF");
            slot.vendor   = array_string(vendors, i);
            if (nfc.is_array() && i < int(nfc.size()) && nfc[i].is_object()) {
                const std::string nfc_vendor = nfc[i].value("VENDOR", "NONE");
                if (!nfc_vendor.empty() && nfc_vendor != "NONE") {
                    slot.bed_temp    = nfc[i].value("BED_TEMP", 0);
                    slot.nozzle_temp = nfc[i].value("FIRST_LAYER_TEMP", 0);
                }
            }
        }
        out.slots.push_back(slot);
    }
    out.source = MoonrakerFilamentState::Source::Snapmaker;
    return true;
}

void resolve_filament_ids(MoonrakerFilamentState &state, const PresetCollection &filaments)
{
    for (MoonrakerFilamentSlot &slot : state.slots) {
        slot.filament_id.clear();
        if (!slot.loaded)
            continue;
        std::string id = id_by_vendor_and_color(filaments, slot.vendor, slot.material, slot.color);
        if (id.empty())
            id = id_by_type(filaments, slot.material);
        if (id.empty()) {
            // "PLA HIGH SPEED" or "PLA SILK" at least share the base material.
            const auto space = slot.material.find(' ');
            if (space != std::string::npos)
                id = id_by_type(filaments, slot.material.substr(0, space));
        }
        slot.filament_id = id.empty() ? UNKNOWN_FILAMENT_ID : id;
    }
}

MoonrakerFilamentState fetch(const DynamicPrintConfig &printer_config, const PresetCollection *filaments, std::string &error)
{
    MoonrakerFilamentState state;
    error.clear();
    const std::string base = base_url(printer_config);
    if (base.empty()) {
        error = "the printer preset has no host";
        return state;
    }

    // The object list tells which changer to ask; a printer that cannot answer it is unreachable.
    nlohmann::json reply;
    unsigned       status = 0;
    if (!http_get_json(base + "/printer/objects/list", printer_config, reply, status, error))
        return state;
    std::set<std::string> objects;
    if (auto result = reply.find("result"); result != reply.end() && result->is_object() && result->contains("objects") && (*result)["objects"].is_array())
        for (const auto &name : (*result)["objects"])
            if (name.is_string())
                objects.insert(name.get<std::string>());

    std::string probe_error;
    if (http_get_json(base + "/server/database/item?namespace=lane_data", printer_config, reply, status, probe_error) && parse_lane_data(reply, state)) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerFilaments: lane_data reports " << state.slots.size() << " lanes";
    } else if (objects.count("mmu") && http_get_json(base + "/printer/objects/query?mmu", printer_config, reply, status, probe_error) && parse_happy_hare(reply, state)) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerFilaments: Happy Hare reports " << state.slots.size() << " gates";
    } else if (objects.count("print_task_config") && http_get_json(base + "/printer/objects/query?print_task_config&filament_detect", printer_config, reply, status, probe_error) &&
               parse_snapmaker(reply, state)) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerFilaments: Snapmaker reports " << state.slots.size() << " slots";
    } else if (objects.count("save_variables") && std::any_of(objects.begin(), objects.end(), [](const std::string &o) { return boost::starts_with(o, "box_stepper slot"); })) {
        std::string url = base + "/printer/objects/query?save_variables=variables";
        for (int i = 0; i < 16; ++i)
            url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
        std::string cfg, cfg_error;
        unsigned    cfg_status = 0;
        http_get(base + "/server/files/config/officiall_filas_list.cfg", printer_config, cfg, cfg_status, cfg_error);
        if (http_get_json(url, printer_config, reply, status, probe_error) && parse_qidi_box(reply, cfg, state))
            BOOST_LOG_TRIVIAL(info) << "MoonrakerFilaments: Qidi box reports " << state.slots.size() << " slots";
    }

    if (state.source == MoonrakerFilamentState::Source::None) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerFilaments: no filament changer at " << base << (probe_error.empty() ? "" : " (" + probe_error + ")");
        return state;
    }
    if (filaments != nullptr)
        resolve_filament_ids(state, *filaments);
    return state;
}

} // namespace MoonrakerFilaments
} // namespace Slic3r
