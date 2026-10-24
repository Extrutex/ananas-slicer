#pragma once

#include "ModelData.hpp"
#include "Preview.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>

namespace Slic3r::engine {

// JSON-RPC 2.0 error codes the engine answers with.
enum class ErrorCode : int {
    ParseError     = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    EngineError    = -32000, // the operation was understood but failed (slicing, import)
    NotFound       = -32001, // a named preset does not exist
    PrinterError   = -32002, // the printer host could not be reached, or refused the request
};

class EngineError : public std::runtime_error
{
public:
    EngineError(ErrorCode code, const std::string& message) : std::runtime_error(message), m_code(code) {}
    ErrorCode code() const { return m_code; }

private:
    ErrorCode m_code;
};

// The headless slicing engine behind a front end: one session over the shipped vendor profiles, driven by
// JSON-RPC requests. It links libslic3r only (no GUI, no Python), and Print::process() keeps process-global
// state, so requests run one at a time.
class Engine
{
public:
    // Reads the vendor profiles under `profiles_dir` (the resources/profiles folder); `vendor` limits the
    // load to one vendor (plus the filament library) when it is not empty. Throws on a load error.
    Engine(const std::string& profiles_dir, const std::string& vendor);

    // Called with a JSON-RPC notification (no id) while a request runs, e.g. slice progress.
    using Notify = std::function<void(const nlohmann::json&)>;

    // Answers one request object with a complete response object. Never throws; a failure becomes the
    // response's "error". Returns null for a request without an id (a notification).
    nlohmann::json handle(const nlohmann::json& request, const Notify& notify);

private:
    nlohmann::json info() const;
    nlohmann::json presets_list(const nlohmann::json& params);
    nlohmann::json config_resolve(const nlohmann::json& params);
    nlohmann::json limits_import(const nlohmann::json& params) const;
    nlohmann::json limits_check(const nlohmann::json& params);
    nlohmann::json model_load(const nlohmann::json& params);
    nlohmann::json model_bounds(const nlohmann::json& params);
    nlohmann::json preview_load(const nlohmann::json& params);
    nlohmann::json preview_chunk(const nlohmann::json& params);
    nlohmann::json moonraker_upload(const nlohmann::json& params) const;
    nlohmann::json moonraker_start(const nlohmann::json& params) const;
    nlohmann::json slice(const nlohmann::json& params, const nlohmann::json& request_id, const Notify& notify);

    // The full config of printer + process + filament, with "overrides" (key -> serialized value) and an
    // imported "limits" object applied. Printer is required; process and filament default to the printer's own.
    DynamicPrintConfig resolve(const nlohmann::json& params);

    PresetBundle m_bundle;
    AppConfig    m_app_config;
    ModelCache   m_models;
    PreviewCache m_preview;
};

} // namespace Slic3r::engine
