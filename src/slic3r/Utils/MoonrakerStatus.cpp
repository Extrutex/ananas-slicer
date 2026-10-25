#include "MoonrakerStatus.hpp"

#include <chrono>

#include <boost/log/trivial.hpp>

#include "Http.hpp"
#include "MoonrakerFilaments.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

bool MoonrakerPrinterStatus::operator==(const MoonrakerPrinterStatus &o) const
{
    return link == o.link && host == o.host && error == o.error && klippy_state == o.klippy_state && print_state == o.print_state &&
           filename == o.filename && message == o.message && progress == o.progress && print_duration == o.print_duration &&
           current_layer == o.current_layer && total_layer == o.total_layer && nozzle_temp == o.nozzle_temp &&
           nozzle_target == o.nozzle_target && has_bed == o.has_bed && bed_temp == o.bed_temp && bed_target == o.bed_target;
}

namespace MoonrakerStatusParser {

const char *QUERY = "webhooks&print_stats&display_status&virtual_sdcard&extruder&heater_bed";

namespace {

double number(const nlohmann::json &obj, const char *key, double fallback)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<double>() : fallback;
}

std::string text(const nlohmann::json &obj, const char *key, const std::string &fallback)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : fallback;
}

} // namespace

bool apply(const nlohmann::json &reply, MoonrakerPrinterStatus &s)
{
    auto result = reply.find("result");
    if (result == reply.end() || !result->is_object())
        return false;
    auto status = result->find("status");
    if (status == result->end() || !status->is_object())
        return false;

    if (auto webhooks = status->find("webhooks"); webhooks != status->end() && webhooks->is_object()) {
        s.klippy_state = text(*webhooks, "state", s.klippy_state);
        if (s.klippy_state != "ready")
            s.message = text(*webhooks, "state_message", s.message);
    }
    if (auto ps = status->find("print_stats"); ps != status->end() && ps->is_object()) {
        s.print_state    = text(*ps, "state", s.print_state);
        s.filename       = text(*ps, "filename", s.filename);
        s.print_duration = number(*ps, "print_duration", s.print_duration);
        if (auto info = ps->find("info"); info != ps->end() && info->is_object()) {
            s.current_layer = int(number(*info, "current_layer", s.current_layer));
            s.total_layer   = int(number(*info, "total_layer", s.total_layer));
        }
    }
    if (auto ds = status->find("display_status"); ds != status->end() && ds->is_object()) {
        if (s.klippy_state == "ready")
            s.message = text(*ds, "message", s.message);
        s.progress = number(*ds, "progress", s.progress);
    }
    if (auto sd = status->find("virtual_sdcard"); sd != status->end() && sd->is_object())
        s.progress = number(*sd, "progress", s.progress);
    if (auto ex = status->find("extruder"); ex != status->end() && ex->is_object()) {
        s.nozzle_temp   = number(*ex, "temperature", s.nozzle_temp);
        s.nozzle_target = number(*ex, "target", s.nozzle_target);
    }
    if (auto bed = status->find("heater_bed"); bed != status->end() && bed->is_object()) {
        s.has_bed    = true;
        s.bed_temp   = number(*bed, "temperature", s.bed_temp);
        s.bed_target = number(*bed, "target", s.bed_target);
    }
    return true;
}

} // namespace MoonrakerStatusParser

void MoonrakerStatus::start(const DynamicPrintConfig &printer_config, Listener listener)
{
    stop();
    const std::string base = MoonrakerFilaments::base_url(printer_config);
    if (base.empty()) {
        MoonrakerPrinterStatus idle;
        listener(idle);
        return;
    }
    const std::string api_key = printer_config.has("printhost_apikey") ? printer_config.opt_string("printhost_apikey") : std::string();
    const std::string ca_file = printer_config.has("printhost_cafile") ? printer_config.opt_string("printhost_cafile") : std::string();
    m_stop.store(false);
    m_thread = std::thread([this, base, api_key, ca_file, listener = std::move(listener)]() { run(base, api_key, ca_file, listener); });
}

void MoonrakerStatus::stop()
{
    {
        // Set under the mutex so a poll between its wait predicate and the wait itself cannot miss the wake-up.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void MoonrakerStatus::run(std::string base_url, std::string api_key, std::string ca_file, Listener listener)
{
    MoonrakerPrinterStatus status;
    status.host = base_url;
    status.link = MoonrakerPrinterStatus::Link::Connecting;
    listener(status);

    const std::string url = base_url + "/printer/objects/query?" + MoonrakerStatusParser::QUERY;
    while (!m_stop.load()) {
        std::string body, error;
        bool        ok = false;
        auto        http = Http::get(url);
        if (!api_key.empty())
            http.header("X-Api-Key", api_key);
        if (!ca_file.empty())
            http.ca_file(ca_file);
        http.timeout_connect(3)
            .timeout_max(5)
            .on_progress([this](Http::Progress, bool &cancel) { cancel = m_stop.load(); })
            .on_complete([&](std::string reply, unsigned) {
                body = std::move(reply);
                ok   = true;
            })
            .on_error([&](std::string, std::string err, unsigned http_status) {
                error = err;
                if (http_status > 0)
                    error += " (HTTP " + std::to_string(http_status) + ")";
            })
            .perform_sync();

        MoonrakerPrinterStatus next = status;
        if (ok) {
            const auto reply = nlohmann::json::parse(body, nullptr, false, true);
            if (reply.is_discarded() || !MoonrakerStatusParser::apply(reply, next)) {
                ok    = false;
                error = "unexpected reply from " + url;
            }
        }
        if (ok) {
            next.link  = MoonrakerPrinterStatus::Link::Online;
            next.error.clear();
        } else {
            next.link  = MoonrakerPrinterStatus::Link::Offline;
            next.error = error;
        }
        if (next != status) {
            status = next;
            listener(status);
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::seconds(ok ? 2 : 5), [this] { return m_stop.load(); });
    }
}

} // namespace Slic3r
