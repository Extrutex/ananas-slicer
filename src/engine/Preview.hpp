#pragma once

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r::engine {

// The toolpaths of a G-code file for a front end: read once, then handed out layer range by layer range as
// typed arrays (base64 inside JSON). One file is kept; a changed file is read again.
class PreviewCache
{
public:
    // Reads the file (unless it is the cached one) and describes it: the layers (index, z), the extent and
    // per-quantity ranges of the extrusions, the roles present, the time estimate and the move counts.
    nlohmann::json load(const std::string& gcode);

    // The segments of the layers `layer_from` to `layer_to` (inclusive, indices into the layer list of load()).
    // Extrusions are always included, travel moves only with `travel`. Per segment: start and end point,
    // speed (mm/s), volumetric flow (mm^3/s), acceleration (mm/s^2), role, kind (0 extrusion, 1 travel) and
    // layer index.
    nlohmann::json chunk(const std::string& gcode, size_t layer_from, size_t layer_to, bool travel);

private:
    struct Layer
    {
        unsigned int id{ 0 };
        float        z{ 0.f };
        size_t       first{ 0 }; // first and one past the last move of the layer
        size_t       end{ 0 };
    };
    struct Entry
    {
        std::string                           path;
        std::time_t                           modified{ 0 };
        std::uintmax_t                        size{ 0 };
        std::shared_ptr<GCodeProcessorResult> result;
        std::vector<Layer>                    layers;
    };

    const Entry& entry_for(const std::string& gcode);

    Entry m_entry;
};

} // namespace Slic3r::engine
