#include "Moonraker.hpp"

#include "Engine.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/KlipperLimits.hpp"

#include <curl/curl.h>

#include <boost/filesystem.hpp>

#include <memory>
#include <mutex>
#include <regex>

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r::engine {

namespace {

constexpr long CONNECT_TIMEOUT_S = 5;
constexpr long REQUEST_TIMEOUT_S = 30;
constexpr long UPLOAD_TIMEOUT_S  = 900;

[[noreturn]] void invalid(const std::string& message)
{
    throw EngineError(ErrorCode::InvalidParams, message);
}

[[noreturn]] void printer_error(const std::string& message)
{
    throw EngineError(ErrorCode::PrinterError, message);
}

void ensure_curl()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t collect(char* data, size_t size, size_t count, void* sink)
{
    static_cast<std::string*>(sink)->append(data, size * count);
    return size * count;
}

struct CurlDeleter
{
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};
struct ListDeleter
{
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};
struct MimeDeleter
{
    void operator()(curl_mime* mime) const { curl_mime_free(mime); }
};

std::string escape(CURL* handle, const std::string& text)
{
    char* escaped = curl_easy_escape(handle, text.c_str(), static_cast<int>(text.size()));
    std::string out = escaped != nullptr ? escaped : "";
    curl_free(escaped);
    return out;
}

// The message Moonraker puts in an error body: {"error": {"message": "..."}}.
std::string moonraker_message(const std::string& body)
{
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_object() && parsed.contains("error")) {
        const json& error = parsed.at("error");
        if (error.is_object() && error.contains("message") && error.at("message").is_string())
            return error.at("message").get<std::string>();
        if (error.is_string())
            return error.get<std::string>();
    }
    return {};
}

} // namespace

MoonrakerTarget parse_target(const json& params)
{
    const json& host = params.is_object() && params.contains("host") ? params.at("host") : json();
    if (!host.is_string() || host.get<std::string>().empty())
        invalid("missing parameter \"host\" (e.g. http://printer.local:7125)");
    std::string url = host.get<std::string>();
    if (url.find("://") == std::string::npos)
        url = "http://" + url;

    static const std::regex pattern(R"(^(https?)://([^/\s?#@]+)((?:/[^?#\s]*)?)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, pattern))
        invalid("\"host\" must be an http or https address without credentials, query or fragment");
    std::string path = match[3].str();
    while (!path.empty() && path.back() == '/')
        path.pop_back();

    MoonrakerTarget target;
    target.base_url = match[1].str() + "://" + match[2].str() + path;
    if (params.contains("api_key")) {
        if (!params.at("api_key").is_string())
            invalid("\"api_key\" must be a string");
        target.api_key = params.at("api_key").get<std::string>();
    }
    return target;
}

Moonraker::Moonraker(MoonrakerTarget target) : m_target(std::move(target))
{
    ensure_curl();
}

std::string Moonraker::request(const std::string& method, const std::string& path, const Query& query, const std::string& file,
                               const std::string& remote_name, bool print) const
{
    std::unique_ptr<CURL, CurlDeleter> handle(curl_easy_init());
    if (!handle)
        printer_error("cannot start an HTTP request");

    std::string url = m_target.base_url + path;
    for (size_t i = 0; i < query.size(); ++i)
        url += (i == 0 ? "?" : "&") + escape(handle.get(), query[i].first) + "=" + escape(handle.get(), query[i].second);

    std::string                           body;
    std::unique_ptr<curl_slist, ListDeleter> headers;
    headers.reset(curl_slist_append(headers.release(), "Accept: application/json"));
    if (!m_target.api_key.empty())
        headers.reset(curl_slist_append(headers.release(), ("X-Api-Key: " + m_target.api_key).c_str()));

    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_S);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, file.empty() ? REQUEST_TIMEOUT_S : UPLOAD_TIMEOUT_S);
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L); // a printer that redirects is not the printer asked for
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);

    std::unique_ptr<curl_mime, MimeDeleter> form;
    if (method == "POST") {
        if (file.empty()) {
            curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, 0L);
        } else {
            form.reset(curl_mime_init(handle.get()));
            curl_mimepart* part = curl_mime_addpart(form.get());
            curl_mime_name(part, "file");
            curl_mime_filedata(part, file.c_str());
            curl_mime_filename(part, remote_name.c_str());
            part = curl_mime_addpart(form.get());
            curl_mime_name(part, "root");
            curl_mime_data(part, "gcodes", CURL_ZERO_TERMINATED);
            part = curl_mime_addpart(form.get());
            curl_mime_name(part, "print");
            curl_mime_data(part, print ? "true" : "false", CURL_ZERO_TERMINATED);
            curl_easy_setopt(handle.get(), CURLOPT_MIMEPOST, form.get());
        }
    }

    const CURLcode code = curl_easy_perform(handle.get());
    if (code != CURLE_OK)
        printer_error("cannot reach " + m_target.base_url + ": " + curl_easy_strerror(code));
    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && status < 300)
        return body;

    const std::string reason = moonraker_message(body);
    if (status == 401 || status == 403)
        printer_error("the printer refused the request (HTTP " + std::to_string(status) + "): check the API key" + (reason.empty() ? "" : " - " + reason));
    printer_error("the printer answered HTTP " + std::to_string(status) + (reason.empty() ? "" : ": " + reason));
}

std::string Moonraker::get_text(const std::string& path, const Query& query) const
{
    return request("GET", path, query, {}, {}, false);
}

json Moonraker::get(const std::string& path, const Query& query) const
{
    const json parsed = json::parse(get_text(path, query), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        printer_error("the printer's answer is not JSON (is this a Moonraker address?)");
    return parsed;
}

json Moonraker::post(const std::string& path, const Query& query) const
{
    const json parsed = json::parse(request("POST", path, query, {}, {}, false), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        printer_error("the printer's answer is not JSON (is this a Moonraker address?)");
    return parsed;
}

json Moonraker::upload(const std::string& file, const std::string& remote_name, bool print) const
{
    if (!fs::is_regular_file(file))
        invalid("G-code file not found: " + file);
    const json parsed = json::parse(request("POST", "/server/files/upload", {}, file, remote_name, print), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        printer_error("the printer's answer to the upload is not JSON");
    return parsed;
}

json moonraker_connect(const MoonrakerTarget& target)
{
    const Moonraker client(target);
    const json      info   = client.get("/server/info");
    const json      result = info.contains("result") ? info.at("result") : info;
    json            out    = { { "host", target.base_url },
                               { "moonraker_version", result.value("moonraker_version", "") },
                               { "klippy_state", result.value("klippy_state", "unknown") },
                               { "klippy_connected", result.value("klippy_connected", false) },
                               { "limits", nullptr },
                               { "warnings", json::array() } };

    // The limits come from the running Klipper's own parse of printer.cfg, so they are only there once it is ready.
    if (out["klippy_state"] != "ready") {
        out["warnings"].push_back("Klipper is not ready (" + out["klippy_state"].get<std::string>() + "); its limits cannot be read yet.");
        return out;
    }
    try {
        const std::string body = client.get_text("/printer/objects/query", { { "toolhead", "" }, { "configfile", "settings" } });
        const KlipperLimitsImport imported = parse_moonraker_toolhead_query(body);
        out["limits"] = { { "max_velocity", imported.limits.max_velocity },
                          { "max_accel", imported.limits.max_accel },
                          { "minimum_cruise_ratio", imported.limits.minimum_cruise_ratio },
                          { "square_corner_velocity", imported.limits.square_corner_velocity },
                          { "max_z_velocity", imported.limits.max_z_velocity },
                          { "max_z_accel", imported.limits.max_z_accel },
                          { "max_extrude_only_velocity", imported.limits.max_extrude_only_velocity },
                          { "max_extrude_only_accel", imported.limits.max_extrude_only_accel },
                          { "instantaneous_corner_velocity", imported.limits.instantaneous_corner_velocity },
                          { "pressure_advance", imported.limits.pressure_advance },
                          { "pressure_advance_smooth_time", imported.limits.pressure_advance_smooth_time } };
        for (const std::string& warning : imported.warnings)
            out["warnings"].push_back(warning);
    } catch (const RuntimeError& ex) {
        out["warnings"].push_back(std::string("The limits could not be read: ") + ex.what());
    }
    return out;
}

json moonraker_status(const MoonrakerTarget& target)
{
    const Moonraker client(target);
    const json      answer = client.get("/printer/objects/query", { { "print_stats", "" }, { "virtual_sdcard", "" }, { "webhooks", "" } });
    const json&     status = answer.contains("result") && answer.at("result").contains("status") ? answer.at("result").at("status") : json::object();
    const json      stats  = status.value("print_stats", json::object());
    const json      sdcard = status.value("virtual_sdcard", json::object());
    const json      hooks  = status.value("webhooks", json::object());
    return { { "state", stats.value("state", "unknown") },
             { "filename", stats.value("filename", "") },
             { "progress", sdcard.value("progress", 0.0) },
             { "print_duration", stats.value("print_duration", 0.0) },
             { "total_duration", stats.value("total_duration", 0.0) },
             { "message", stats.value("message", "") },
             { "klippy_state", hooks.value("state", "unknown") } };
}

} // namespace Slic3r::engine
