#include "Moonraker.hpp"

#include <sstream>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#ifdef WIN32
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"
#endif

namespace pt = boost::property_tree;

namespace Slic3r {

namespace {

// Replaces the host part of `orig_addr` by `sub_addr` using libcurl's URL API. Any failure
// returns the input unchanged, which falls back to the system resolver.
std::string substitute_host(const std::string &orig_addr, std::string sub_addr)
{
    // an IPv6 address must be bracketed inside a URL
    if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[')
        sub_addr = "[" + sub_addr + "]";

    std::string out = orig_addr;
    CURLU *hurl = curl_url();
    if (hurl == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to allocate curl_url";
        return out;
    }
    CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
    if (rc == CURLUE_OK) {
        rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
        if (rc == CURLUE_OK) {
            char *url = nullptr;
            rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
            if (rc == CURLUE_OK) {
                out = url;
                curl_free(url);
            } else
                BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to extract the URL after substitution";
        } else
            BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to substitute host " << sub_addr << " in URL " << orig_addr;
    } else
        BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to parse URL " << orig_addr;
    curl_url_cleanup(hurl);
    return out;
}

// A print host is built from a physical printer's config. Configs assembled elsewhere (CLI,
// tests, a full print config) may lack the printhost_* options; those then take their
// documented defaults instead of dereferencing a missing option.
template<class T> const T &host_option(const DynamicPrintConfig &config, const char *key)
{
    if (const T *opt = config.option<T>(key))
        return *opt;
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr || def->get_default_value<T>() == nullptr)
        throw Slic3r::RuntimeError(std::string("Moonraker: unknown print host option ") + key);
    return *def->get_default_value<T>();
}

} // namespace

Moonraker::Moonraker(DynamicPrintConfig *config)
    : m_host(host_option<ConfigOptionString>(*config, "print_host").value)
    , m_apikey(host_option<ConfigOptionString>(*config, "printhost_apikey").value)
    , m_cafile(host_option<ConfigOptionString>(*config, "printhost_cafile").value)
    , m_ssl_revoke_best_effort(host_option<ConfigOptionBool>(*config, "printhost_ssl_ignore_revoke").value)
{}

std::string Moonraker::uploaded_path(const std::string &response_body)
{
    // Moonraker answers /server/files/upload without the {"result": ...} envelope of its other
    // endpoints: {"item": {"path": "<dir>/<name>.gcode", "root": "gcodes"}, "action": ...}.
    // The enveloped form is accepted too, for proxies and compatible servers that add it.
    std::stringstream ss(response_body);
    pt::ptree ptree;
    pt::read_json(ss, ptree);
    if (const auto path = ptree.get_optional<std::string>("item.path"))
        return *path;
    if (const auto path = ptree.get_optional<std::string>("result.item.path"))
        return *path;
    return {};
}

bool Moonraker::upload_took_print(const std::string &response_body)
{
    // Same shape as for uploaded_path(): top-level on Moonraker itself, enveloped on proxies.
    std::stringstream ss(response_body);
    pt::ptree ptree;
    pt::read_json(ss, ptree);
    for (const char *prefix : {"", "result."})
        if (ptree.get<bool>(std::string(prefix) + "print_started", false) ||
            ptree.get<bool>(std::string(prefix) + "print_queued", false))
            return true;
    return false;
}

const char* Moonraker::get_name() const { return "Moonraker"; }

wxString Moonraker::get_test_ok_msg() const
{
    return _(L("Connection to Moonraker is working correctly."));
}

wxString Moonraker::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Moonraker"), msg);
}

std::string Moonraker::make_url(const std::string &path, const std::string &resolved_ip) const
{
    std::string url;
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/')
            url = (boost::format("%1%%2%") % m_host % path).str();
        else
            url = (boost::format("%1%/%2%") % m_host % path).str();
    } else {
        url = (boost::format("http://%1%/%2%") % m_host % path).str();
    }
    return resolved_ip.empty() ? url : substitute_host(url, resolved_ip);
}

void Moonraker::set_auth(Http &http, const std::string &resolved_ip) const
{
    // Moonraker accepts unauthenticated requests by default; X-Api-Key is the only auth header
    // defined by the Moonraker spec. HTTP Basic / Digest do NOT belong here even if the user
    // filled the user/password fields: those are PrusaLink/OctoPrint conventions.
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);
    // With a resolved IP in the URL libcurl would send that IP as the Host header. Send the
    // host name the user typed instead, so a reverse proxy in front of Moonraker still routes
    // the request by name (RFC 7230 section 5.4).
    if (!resolved_ip.empty())
        http.header("Host", Http::get_host_header_value(m_host));
}

bool Moonraker::check_server_info(const std::string &resolved_ip, wxString &msg, std::string *resolved_ip_out) const
{
    // /server/info returns
    //     { "result": { "klippy_state": "ready|startup|shutdown|error|disconnected", ... } }
    // The connection counts as healthy as long as the envelope is valid and `klippy_state`
    // is present, matching the "can I reach this host?" convention of the other hosts.
    // Klipper's own state is logged but does not gate the test: buddy-fork firmwares
    // legitimately report non-`ready` states at idle, and a real problem surfaces as a
    // contextual error at upload() time anyway.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("server/info", resolved_ip);

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get server info at: %2%") % name % url;

    auto http = Http::get(url);
    set_auth(http, resolved_ip);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting server info at %2%: %3%, HTTP %4%, body: `%5%`")
            % name % url % error % status % body;
        res = false;
        msg = format_error(body, error, status);
    })
    .on_complete([&](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/info body: %2%") % name % body;
        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);

            const auto klippy_state = ptree.get_optional<std::string>("result.klippy_state");
            if (!klippy_state) {
                // The response is not shaped like a Moonraker /server/info reply: most likely an
                // OctoPrint or PrusaLink host, or a totally different service, behind this address.
                res = false;
                msg = _L("The host responded but it doesn't look like Moonraker (missing result.klippy_state).");
                return;
            }
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: klippy_state = %2%") % name % (*klippy_state);
        } catch (const std::exception &ex) {
            res = false;
            msg = GUI::format_wxstr(_L("Could not parse Moonraker server response: %s"), ex.what());
        }
    });
    if (resolved_ip_out != nullptr) {
        // Remember the address libcurl connected to, so the upload can reuse it instead of
        // resolving the host name a second time.
        http.on_ip_resolve([resolved_ip_out](std::string address) { *resolved_ip_out = std::move(address); });
    }
#ifdef WIN32
    http.ssl_revoke_best_effort(m_ssl_revoke_best_effort);
#endif
    http.perform_sync();

    return res;
}

bool Moonraker::test(wxString &msg) const
{
    return check_server_info({}, msg, nullptr);
}

bool Moonraker::get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const
{
    // GET /server/files/roots enumerates Moonraker's storage roots (default "gcodes" plus any
    // configured extras like "config", "logs", "timelapse"). Only roots with write permission
    // can receive uploads, so the dropdown offers just those. Failures (404 on an older
    // Moonraker, 501 on a slimmer shim) degrade to false and upload() falls back to "gcodes".
    const char *name = get_name();
    bool got_any = false;
    auto url = make_url("server/files/roots");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Enumerating storage roots at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        if (status == 404 || status == 501) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots not implemented (HTTP %2%); upload() will fall back to the \"gcodes\" root.")
                % name % status;
        } else {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not enumerate roots: %2%, HTTP %3%, body: `%4%`")
                % name % error % status % body;
        }
    })
    .on_complete([&](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots body: %2%") % name % body;
        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);
            const auto result_node = ptree.get_child_optional("result");
            if (!result_node)
                return;
            for (const auto &child : *result_node) {
                const std::string &root = child.second.get<std::string>("name", "");
                const std::string &perms = child.second.get<std::string>("permissions", "");
                if (root.empty() || perms.find('w') == std::string::npos)
                    continue;
                storage_path.Add(wxString::FromUTF8(root));
                storage_name.Add(wxString::FromUTF8(root));
                got_any = true;
            }
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not parse roots: %2%") % name % ex.what();
        }
    })
#ifdef WIN32
    .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
    .perform_sync();

    return got_any;
}

bool Moonraker::start_print(const std::string &resolved_ip, wxString &error_msg, const std::string &filename) const
{
    // POST /printer/print/start with JSON body { "filename": "<name>.gcode" }.
    // `filename` is what /server/files/upload returned as item.path (the storage-relative
    // path inside `root`, no leading slash, with extension). The body goes through property_tree
    // so that special characters in the file name are escaped properly.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("printer/print/start", resolved_ip);
    pt::ptree body_tree;
    body_tree.put("filename", filename);
    std::ostringstream body_ss;
    pt::write_json(body_ss, body_tree, /*pretty=*/false);
    std::string body = body_ss.str();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Starting print of %2% at %3%") % name % filename % url;

    auto http = Http::post(url);
    set_auth(http, resolved_ip);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: print/start HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error starting print at %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            res = false;
            error_msg = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

bool Moonraker::upload_inner(const std::string &resolved_ip, PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    // POST /server/files/upload as multipart/form-data with:
    //     file  = <gcode file>
    //     root  = <storage root>     (Moonraker default: "gcodes")
    //     print = true|false         (true for "upload and print")
    // Successful response shape (no "result" envelope on this endpoint):
    //     { "item": { "path": "<name>.gcode", "root": "<root>" }, "print_started": <bool>,
    //       "print_queued": <bool>, "action": "create_file" }
    // With print=true Moonraker starts the print itself. A separate /printer/print/start right
    // after the upload fails with HTTP 503 when Moonraker is still bringing Klippy up, for
    // example after a [power] device switched the printer on for the queued job; Moonraker's
    // own start waits for Klippy instead. print_started or print_queued means Moonraker owns
    // the print. Only when it reports neither (no job queue, or a server that ignores `print`)
    // is the print started explicitly below. (Backport of OrcaSlicer #15032.)
    const char *name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    // The storage comes from the upload dialog's root picker (get_storage). When unset, fall
    // back to the Moonraker-standard "gcodes" root.
    const std::string root = upload_data.storage.empty() ? std::string("gcodes") : upload_data.storage;

    std::string url = make_url("server/files/upload", resolved_ip);
    bool result = true;
    std::string stored_path;
    const bool want_start = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    bool moonraker_took_print = false;

    if (!resolved_ip.empty())
        info_fn(L"resolve", boost::nowide::widen(url));

    // gcode inside a .gcode.3mf is index-coded (Metadata/plate_<N>.gcode), so the upload names the
    // plate via a 1-based `plateindex` (set only in the .3mf path, see Plater::send_gcode_legacy);
    // servers that don't use it ignore the unknown form field.
    const std::string plateindex = upload_data.extended("plateindex");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% to %3% (root=%4%, filename=%5%, plateindex=%6%, start_print=%7%)")
        % name
        % upload_data.source_path
        % url
        % root
        % upload_filename.string()
        % (plateindex.empty() ? "-" : plateindex)
        % (want_start ? "true" : "false");

    auto http = Http::post(url);
    set_auth(http, resolved_ip);
    http.form_add("root", root);
    if (!plateindex.empty())
        http.form_add("plateindex", plateindex);
    http.form_add("print", want_start ? "true" : "false");
    http.form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: upload HTTP %2%: %3%") % name % status % body;
            try {
                // Moonraker confirms the storage-relative path in item.path. Exactly that string
                // goes to /printer/print/start, so any server-side renaming is respected.
                stored_path = Moonraker::uploaded_path(body);
                if (stored_path.empty()) {
                    stored_path = upload_filename.string();
                    BOOST_LOG_TRIVIAL(warning) << boost::format(
                        "%1%: upload response has no item.path, falling back to original filename `%2%`")
                        % name % stored_path;
                }
                moonraker_took_print = Moonraker::upload_took_print(body);
                if (want_start)
                    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Moonraker %2% the print itself")
                        % name % (moonraker_took_print ? "started or queued" : "did not start");
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << boost::format(
                    "%1%: could not parse upload response (%2%); falling back to original filename")
                    % name % ex.what();
                stored_path = upload_filename.string();
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading to %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (!result)
        return false;

    // Fallback only when Moonraker neither started nor queued the print. If the printer cannot
    // start it, the error from this request is what the user sees.
    if (want_start && !stored_path.empty() && !moonraker_took_print) {
        wxString start_msg;
        if (!start_print(resolved_ip, start_msg, stored_path)) {
            error_fn(std::move(start_msg));
            return false;
        }
    }
    return true;
}

#ifdef WIN32
bool Moonraker::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address &resolved_addr) const
{
    const std::string resolved_ip = resolved_addr.to_string();
    info_fn(L"resolve", boost::nowide::widen(resolved_ip));

    wxString test_msg;
    if (!check_server_info(resolved_ip, test_msg, nullptr)) {
        error_fn(std::move(test_msg));
        return false;
    }
    return upload_inner(resolved_ip, std::move(upload_data), progress_fn, error_fn, info_fn);
}
#endif // WIN32

bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
#ifndef WIN32
    wxString test_msg;
    if (!check_server_info({}, test_msg, nullptr)) {
        error_fn(std::move(test_msg));
        return false;
    }
    return upload_inner({}, std::move(upload_data), progress_fn, error_fn, info_fn);
#else
    // Windows 10/11 cannot resolve the same mDNS name twice in quick succession. A `.local`
    // host is therefore resolved once here, or the address of the connection test is reused,
    // and every following request goes to that IP with the user's host name in the Host header.
    // An https address is never substituted: the certificate is bound to the name, not the IP.
    const std::string host = Http::get_host_from_url(m_host);
    const bool allow_ip_resolve = GUI::get_app_config()->get_bool("allow_ip_resolve") && m_host.find("https://") != 0;

    std::vector<boost::asio::ip::address> resolved_addr;
    boost::system::error_code ec;
    boost::asio::ip::address host_ip = boost::asio::ip::make_address(host, ec);
    if (!ec) {
        resolved_addr.push_back(host_ip);
    } else if (allow_ip_resolve && boost::algorithm::ends_with(host, ".local")) {
        Bonjour("moonraker")
            .set_hostname(host)
            .set_retries(5) // number of rounds of queries sent
            .set_timeout(1) // after each timeout, if there is any answer, the resolving stops
            .on_resolve([&ra = resolved_addr](const std::vector<BonjourReply> &replies) {
                for (const auto &rpl : replies) {
                    boost::asio::ip::address ip(rpl.ip);
                    ra.emplace_back(ip);
                    BOOST_LOG_TRIVIAL(info) << "Resolved IP address: " << rpl.ip;
                }
            })
            .resolve_sync();
    }

    if (resolved_addr.empty()) {
        // Nothing resolved by mDNS: let the connection test resolve the name through the system
        // resolver and reuse the address libcurl connected to for the upload.
        BOOST_LOG_TRIVIAL(info) << "Moonraker: no mDNS answer for " << m_host << ", using the system resolver.";
        wxString test_msg;
        std::string connected_ip;
        if (!check_server_info({}, test_msg, allow_ip_resolve ? &connected_ip : nullptr)) {
            error_fn(std::move(test_msg));
            return false;
        }
        if (!connected_ip.empty()) {
            info_fn(L"resolve", boost::nowide::widen(connected_ip));
            BOOST_LOG_TRIVIAL(info) << "Moonraker: upload address after ip resolve: " << connected_ip;
        }
        return upload_inner(connected_ip, std::move(upload_data), progress_fn, error_fn, info_fn);
    }
    if (resolved_addr.size() == 1)
        return upload_inner_with_resolved_ip(std::move(upload_data), progress_fn, error_fn, info_fn, resolved_addr.front());
    if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
        // One IPv4 and one IPv6 address: try both, and report both errors if both fail.
        wxString error_message;
        if (!upload_inner_with_resolved_ip(upload_data, progress_fn
            , [&msg = error_message, resolved_addr](wxString error) { msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); }
            , info_fn, resolved_addr.front())
            &&
            !upload_inner_with_resolved_ip(upload_data, progress_fn
            , [&msg = error_message, resolved_addr](wxString error) { msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error); }
            , info_fn, resolved_addr.back())
            ) {
            error_fn(error_message);
            return false;
        }
        return true;
    }
    // Several addresses: the user picks one.
    size_t selected_index = resolved_addr.size();
    IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
    if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size())
        return upload_inner_with_resolved_ip(std::move(upload_data), progress_fn, error_fn, info_fn, resolved_addr[selected_index]);
    return false;
#endif // WIN32
}

}
