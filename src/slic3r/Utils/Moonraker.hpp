#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <string>
#include <wx/string.h>
#include <wx/arrstr.h>

#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Moonraker is the JSON / WebSocket gateway that ships in front of Klipper
// (and on Klipper-API-compatible firmwares like the Prusa-Firmware-Buddy
// Buddy-Klipper fork). REST shape differs from OctoPrint: distinct paths,
// JSON body for print/start, {"result":...}/{"error":...} envelope.
//
// Endpoints used:
//   GET  /server/info                      -- connection test, reads klippy_state
//   GET  /server/files/roots               -- storage roots for the upload dialog
//   POST /server/files/upload (multipart)  -- upload gcode (form fields: file, root)
//   POST /printer/print/start (json)       -- {"filename":"<name>.gcode"} starts print
//
// Auth: X-Api-Key header if `printhost_apikey` is non-empty; Moonraker accepts
// unauthenticated LAN access by default, so the key is optional. HTTP Basic /
// Digest are not part of the Moonraker spec and are not sent.
//
// Discovery: Moonraker's zeroconf component announces every instance as
// `_moonraker._tcp` on the local network, which is what the "Browse" dialog
// looks for. On Windows a `.local` host name is resolved once through mDNS
// and the resolved address is reused for the whole upload, because two mDNS
// resolves in quick succession fail there (Windows 10/11 resolver bug).
class Moonraker : public PrintHost
{
public:
    Moonraker(DynamicPrintConfig *config);
    ~Moonraker() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    bool get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const override;
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }
    // Storage-relative path of the uploaded file from a /server/files/upload response body, or
    // empty if the response carries none. Throws if the body is not JSON.
    static std::string uploaded_path(const std::string &response_body);
    // Whether Moonraker took over the print of an upload sent with print=true: the
    // /server/files/upload response reports print_started (it began the print) or print_queued
    // (it put the job in its queue, e.g. while a [power] device brings the printer up).
    // False for a response with neither flag. Throws if the body is not JSON.
    static bool upload_took_print(const std::string &response_body);

protected:
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    // Builds the request URL for `path`. With a non-empty `resolved_ip` the host part of the
    // user's address is replaced by that IP and the request carries the user's host name in
    // its Host header instead (see set_auth), so that reverse proxies keep routing by name.
    std::string make_url(const std::string &path, const std::string &resolved_ip = {}) const;
    void set_auth(Http &http, const std::string &resolved_ip = {}) const;

    // GET /server/info. On Windows `resolved_ip_out`, when given, receives the address
    // libcurl actually connected to, so the following requests can reuse it.
    bool check_server_info(const std::string &resolved_ip, wxString &msg, std::string *resolved_ip_out) const;
    bool start_print(const std::string &resolved_ip, wxString &error_msg, const std::string &filename) const;
    // Connection test, upload and print start against one resolved address (empty: the host as
    // typed, resolved by the system resolver).
    bool upload_inner(const std::string &resolved_ip, PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const;
#ifdef WIN32
    bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address &resolved_addr) const;
#endif
};

}

#endif
