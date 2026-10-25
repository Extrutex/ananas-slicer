#include "Preview.hpp"

#include "Base64.hpp"
#include "Engine.hpp"
#include "ModelData.hpp"

#include "libslic3r/ExtrusionEntity.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r::engine {

namespace {

using Move = GCodeProcessorResult::MoveVertex;

enum SegmentKind : uint8_t { KindExtrusion = 0, KindTravel = 1 };

// What the toolhead really did: the feedrate after the machine limits, falling back to the commanded one.
float speed_of(const Move& move)
{
    return move.actual_feedrate > 0.f ? move.actual_feedrate : move.feedrate;
}

float flow_of(const Move& move)
{
    return speed_of(move) * move.mm3_per_mm;
}

bool is_drawn(const Move& previous, const Move& move, bool travel)
{
    const bool extrusion = move.type == EMoveType::Extrude && move.extrusion_role != erNone;
    const bool moving    = move.type == EMoveType::Travel && travel;
    return (extrusion || moving) && (move.position - previous.position).squaredNorm() > 1e-8f;
}

template<class T> std::string encode(const std::vector<T>& values)
{
    return base64_encode(values.data(), values.size() * sizeof(T));
}

struct Range
{
    float lo{ std::numeric_limits<float>::max() };
    float hi{ std::numeric_limits<float>::lowest() };

    void add(float value)
    {
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    json to_json() const { return lo <= hi ? json{ { "min", lo }, { "max", hi } } : json{ { "min", 0.0 }, { "max", 0.0 } }; }
};

} // namespace

const PreviewCache::Entry& PreviewCache::entry_for(const std::string& gcode)
{
    boost::system::error_code ec;
    const fs::path            file(gcode);
    if (!fs::is_regular_file(file, ec))
        throw EngineError(ErrorCode::InvalidParams, "G-code file not found: " + gcode);
    const std::time_t    modified = fs::last_write_time(file, ec);
    const std::uintmax_t size     = fs::file_size(file, ec);
    if (m_entry.result && m_entry.path == gcode && m_entry.modified == modified && m_entry.size == size)
        return m_entry;

    Entry entry;
    entry.path     = gcode;
    entry.modified = modified;
    entry.size     = size;
    try {
        GCodeProcessor processor;
        processor.init_filament_maps_and_nozzle_type_when_import_only_gcode();
        GCodeProcessor::s_IsBBLPrinter = false;
        processor.process_file(gcode);
        entry.result = std::make_shared<GCodeProcessorResult>();
        *entry.result = std::move(processor.extract_result());
    } catch (const std::exception& ex) {
        throw EngineError(ErrorCode::EngineError, std::string("cannot read the G-code: ") + ex.what());
    }

    // Layers that carry extrusions, in file order, with the range of moves that belongs to each.
    const std::vector<Move>& moves = entry.result->moves;
    for (size_t i = 1; i < moves.size(); ++i) {
        const Move& move = moves[i];
        if (move.type != EMoveType::Extrude || move.extrusion_role == erNone)
            continue;
        if (entry.layers.empty() || entry.layers.back().id != move.layer_id) {
            if (!entry.layers.empty())
                entry.layers.back().end = i;
            entry.layers.push_back({ move.layer_id, move.position.z(), i, i + 1 });
        }
        entry.layers.back().end = i + 1;
    }
    // A layer's travel moves sit between its first and last extrusion; the next layer starts at its first extrusion.
    for (size_t k = 0; k + 1 < entry.layers.size(); ++k)
        entry.layers[k].end = entry.layers[k + 1].first;
    if (!entry.layers.empty())
        entry.layers.back().end = moves.size();

    m_entry = std::move(entry);
    return m_entry;
}

json PreviewCache::load(const std::string& gcode)
{
    const Entry&             entry = entry_for(gcode);
    const std::vector<Move>& moves = entry.result->moves;

    Range                      speed, flow, accel;
    BoundingBoxf3              box;
    size_t                     extrusions = 0, travels = 0;
    std::map<uint8_t, size_t>  roles;
    for (size_t i = 1; i < moves.size(); ++i) {
        const Move& move = moves[i];
        if (is_drawn(moves[i - 1], move, false)) {
            ++extrusions;
            speed.add(speed_of(move));
            flow.add(flow_of(move));
            accel.add(move.acceleration);
            box.merge(move.position.cast<double>());
            box.merge(moves[i - 1].position.cast<double>());
            ++roles[uint8_t(move.extrusion_role)];
        } else if (move.type == EMoveType::Travel && (move.position - moves[i - 1].position).squaredNorm() > 1e-8f) {
            ++travels;
        }
    }

    json layers = json::array();
    for (const Layer& layer : entry.layers)
        layers.push_back({ { "id", layer.id }, { "z", layer.z } });
    json role_list = json::array();
    for (const auto& [role, count] : roles)
        role_list.push_back({ { "id", role }, { "name", ExtrusionEntity::role_to_string(ExtrusionRole(role)) }, { "segments", count } });

    return { { "gcode", gcode },
             { "time_s", entry.result->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time },
             { "layers", std::move(layers) },
             { "extrusion_segments", extrusions },
             { "travel_segments", travels },
             { "bbox", box.defined ? bounding_box_json(box) : json(nullptr) },
             { "speed", speed.to_json() },
             { "flow", flow.to_json() },
             { "accel", accel.to_json() },
             { "roles", std::move(role_list) } };
}

json PreviewCache::chunk(const std::string& gcode, size_t layer_from, size_t layer_to, bool travel)
{
    const Entry& entry = entry_for(gcode);
    if (entry.layers.empty())
        return { { "count", 0 } };
    if (layer_from > layer_to || layer_to >= entry.layers.size())
        throw EngineError(ErrorCode::InvalidParams, "layer range is outside 0.." + std::to_string(entry.layers.size() - 1));

    const std::vector<Move>& moves = entry.result->moves;
    std::vector<float>    positions, speeds, flows, accels;
    std::vector<uint8_t>  roles, kinds;
    std::vector<uint16_t> layer_index;
    for (size_t layer = layer_from; layer <= layer_to; ++layer) {
        for (size_t i = std::max<size_t>(entry.layers[layer].first, 1); i < entry.layers[layer].end; ++i) {
            const Move& move = moves[i];
            const Move& from = moves[i - 1];
            if (!is_drawn(from, move, travel))
                continue;
            positions.insert(positions.end(), { from.position.x(), from.position.y(), from.position.z(), move.position.x(),
                                                move.position.y(), move.position.z() });
            const bool extrusion = move.type == EMoveType::Extrude;
            speeds.push_back(speed_of(move));
            flows.push_back(extrusion ? flow_of(move) : 0.f);
            accels.push_back(move.acceleration);
            roles.push_back(extrusion ? uint8_t(move.extrusion_role) : 0);
            kinds.push_back(extrusion ? KindExtrusion : KindTravel);
            layer_index.push_back(uint16_t(layer));
        }
    }
    return { { "count", speeds.size() },
             { "positions", encode(positions) },
             { "speed", encode(speeds) },
             { "flow", encode(flows) },
             { "accel", encode(accels) },
             { "role", encode(roles) },
             { "kind", encode(kinds) },
             { "layer", encode(layer_index) } };
}

} // namespace Slic3r::engine
