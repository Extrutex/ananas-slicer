#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Slic3r::engine {

// A printer host that speaks Moonraker's HTTP API (Moonraker itself, or FLUXRAKER).
struct MoonrakerTarget
{
    std::string base_url; // "http://host:7125", no trailing slash
    std::string api_key;  // sent as X-Api-Key when not empty; never logged
};

// Reads "host" and "api_key" from request parameters. A host without a scheme is http; only http and https are
// accepted, and credentials in the URL, a query or a fragment are refused (the key goes in "api_key").
// Throws EngineError (invalid params) otherwise.
MoonrakerTarget parse_target(const nlohmann::json& params);

using Query = std::vector<std::pair<std::string, std::string>>;

// One blocking HTTP exchange. Redirects are not followed, certificates are verified, and the answer is the
// parsed JSON body. A host that cannot be reached, a refused key or an HTTP error becomes an EngineError of
// code PrinterError with the host's own message.
class Moonraker
{
public:
    explicit Moonraker(MoonrakerTarget target);

    nlohmann::json get(const std::string& path, const Query& query = {}) const;
    nlohmann::json post(const std::string& path, const Query& query = {}) const;
    // multipart/form-data upload to /server/files/upload: root "gcodes", `print` starts it when stored.
    nlohmann::json upload(const std::string& file, const std::string& remote_name, bool print) const;

    // The raw body of a GET, for the importer that parses it itself.
    std::string get_text(const std::string& path, const Query& query = {}) const;

    const MoonrakerTarget& target() const { return m_target; }

private:
    std::string request(const std::string& method, const std::string& path, const Query& query, const std::string& file,
                        const std::string& remote_name, bool print) const;

    MoonrakerTarget m_target;
};

// What the front end shows of a printer: the host's and Klipper's state, and the Klipper limits as printer.cfg
// resolves them (null while Klipper is not ready).
nlohmann::json moonraker_connect(const MoonrakerTarget& target);

// The state of the current print: {"state", "filename", "progress" (0..1), "print_duration", "total_duration", "message"}.
nlohmann::json moonraker_status(const MoonrakerTarget& target);

} // namespace Slic3r::engine
