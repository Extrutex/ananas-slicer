#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

#include <wx/arrstr.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Moonraker.hpp"

// Invoked explicitly by FLUXRAKER's virtual-printer runner. The endpoint must
// be a Docker port bound to numeric loopback; never accept a real printer URL.
TEST_CASE("The Moonraker host uploads and starts a print on the virtual API", "[Moonraker][VirtualPrinter][.]")
{
    const char* url   = std::getenv("ANANAS_SLICER_VIRTUAL_HOST_URL");
    const char* key   = std::getenv("ANANAS_SLICER_VIRTUAL_API_KEY");
    const char* gcode = std::getenv("ANANAS_SLICER_VIRTUAL_GCODE");
    if (url == nullptr || key == nullptr || gcode == nullptr)
        SKIP("Run through FLUXRAKER tests/virtual-printer/run.py with the isolated rig.");

    REQUIRE(std::regex_match(url, std::regex(R"(http://127\.0\.0\.1:[0-9]+)")));
    REQUIRE(std::string(key).size() > 0);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"print_host", url}, {"printhost_apikey", key}, {"printhost_cafile", ""},
                                   {"printhost_ssl_ignore_revoke", "0"}});
    Slic3r::Moonraker host(&config);
    wxString error;
    const bool connected = host.test(error);
    INFO(error.ToStdString());
    REQUIRE(connected);

    wxArrayString roots, names;
    REQUIRE(host.get_storage(roots, names));
    REQUIRE(roots.Index("gcodes") != wxNOT_FOUND);

    Slic3r::PrintHostUpload upload;
    upload.source_path = gcode;
    upload.upload_path = "ananas-slicer virtual cube.gcode";
    upload.storage = "gcodes";
    upload.post_action = Slic3r::PrintHostPostUploadAction::StartPrint;
    std::vector<std::string> errors;
    const bool uploaded = host.upload(
        std::move(upload), [](Slic3r::Http::Progress, bool&) {},
        [&errors](wxString message) { errors.push_back(message.ToStdString()); },
        [](wxString, wxString) {});
    CAPTURE(errors);
    CHECK(errors.empty());
    REQUIRE(uploaded);
}
