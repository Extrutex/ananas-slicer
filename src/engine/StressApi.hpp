#pragma once

#include "Stress.hpp"

#include "libslic3r/Model.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r::engine {

// Builds the solver input of an `analyze.stress` request: the model parts of object `params.object` (default 0)
// placed with `params.transform` (same shape as a slice transform; the bed frame, z = 0 on the bed), the
// material, the voxel size, the supports and the loads. Throws EngineError (invalid params) on a wrong shape,
// a non-finite number or an object that does not exist. Reasons to doubt the model (negative volumes, more than
// one instance) go into `warnings` of the input.
StressInput parse_stress_request(const nlohmann::json& params, Model& model);

// The summary the request answers with: grid, counts, solver state, maxima and where they are.
nlohmann::json stress_summary_json(size_t id, const StressResult& result);

// The cell data of the grid layers `z_from` to `z_to` (inclusive), x fastest: solid (uint8), von_mises, safety
// and displacement (float32), displacement_vec (float32 x3), as base64 inside JSON. At most `max_bytes` of raw
// data (25 bytes per cell) per call; a larger range answers invalid params with the number of layers that fit.
nlohmann::json stress_chunk_json(size_t id, const StressResult& result, size_t z_from, size_t z_to,
                                 size_t max_bytes = STRESS_CHUNK_MAX_BYTES);

} // namespace Slic3r::engine
