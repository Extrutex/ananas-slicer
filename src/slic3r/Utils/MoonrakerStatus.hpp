#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// What the Device tab shows above the web view: the state of the printer behind the
// printer preset, as Moonraker reports it.
struct MoonrakerPrinterStatus
{
    enum class Link { Idle, Connecting, Online, Offline };

    Link        link = Link::Idle;   // Idle: the preset has no host
    std::string host;                // the base URL polled
    std::string error;               // the transport error while Offline
    std::string klippy_state;        // webhooks.state: ready, startup, shutdown, error
    std::string print_state;         // print_stats.state: standby, printing, paused, complete, cancelled, error
    std::string filename;            // print_stats.filename
    std::string message;             // display_status.message, or webhooks.state_message while Klipper is not ready
    double      progress       = 0;  // 0..1, virtual_sdcard.progress with display_status.progress as fallback
    double      print_duration = 0;  // seconds printed so far
    int         current_layer  = -1; // print_stats.info.current_layer, -1 when not reported
    int         total_layer    = -1; // print_stats.info.total_layer, -1 when not reported
    double      nozzle_temp    = 0;
    double      nozzle_target  = 0;
    bool        has_bed        = false;
    double      bed_temp       = 0;
    double      bed_target     = 0;

    bool operator==(const MoonrakerPrinterStatus &o) const;
    bool operator!=(const MoonrakerPrinterStatus &o) const { return !(*this == o); }
};

namespace MoonrakerStatusParser {
// The printer objects one poll asks for, as the query string of /printer/objects/query.
extern const char *QUERY;
// Applies a /printer/objects/query reply (result.status) to `status`; false when the reply has
// no status object. Fields the reply leaves out keep their value.
bool apply(const nlohmann::json &reply, MoonrakerPrinterStatus &status);
} // namespace MoonrakerStatusParser

// Polls the printer behind a printer preset on a background thread and hands every change to
// the listener on that thread. The caller marshals to the GUI. Polling runs every 2 s while the
// printer answers and every 5 s while it does not.
class MoonrakerStatus
{
public:
    using Listener = std::function<void(const MoonrakerPrinterStatus &)>;

    MoonrakerStatus() = default;
    ~MoonrakerStatus() { stop(); }
    MoonrakerStatus(const MoonrakerStatus &)            = delete;
    MoonrakerStatus &operator=(const MoonrakerStatus &) = delete;

    // Stops a running poll and starts one for this preset's host. A preset without a host only
    // tells the listener Link::Idle once.
    void start(const DynamicPrintConfig &printer_config, Listener listener);
    void stop();

private:
    void run(std::string base_url, std::string api_key, std::string ca_file, Listener listener);

    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};
    std::mutex              m_mutex;
    std::condition_variable m_wake;
};

} // namespace Slic3r
