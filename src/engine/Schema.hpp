#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r::engine {

// The setting definitions of the FFF print, filament and printer presets as JSON: for every key the
// scope it belongs to, its type, label, tooltip, category, unit, bounds, enum choices and default.
// This is what a front end builds its parameter rows from; the engine owns the definitions, so a UI
// never carries a copy of them.
nlohmann::json config_schema();

} // namespace Slic3r::engine
