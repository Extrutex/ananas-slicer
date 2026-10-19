#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Moonraker.hpp"

using Slic3r::Moonraker;

TEST_CASE("The Moonraker upload response yields the stored path", "[Moonraker]")
{
    SECTION("Moonraker's own response has no result envelope") {
        // Recorded from Moonraker v0.11.0 on the virtual printer rig, path in a subdirectory.
        const std::string body = R"({"action":"create_file","item":{"modified":1792056844.82,"size":335653,)"
                                 R"("permissions":"rw","path":"parts/ananas-slicer cube.gcode","root":"gcodes"},)"
                                 R"("print_started":false,"print_queued":false})";
        CHECK(Moonraker::uploaded_path(body) == "parts/ananas-slicer cube.gcode");
    }
    SECTION("an enveloped response is accepted") {
        CHECK(Moonraker::uploaded_path(R"({"result":{"item":{"path":"a/b.gcode","root":"gcodes"}}})") == "a/b.gcode");
    }
    SECTION("a response without a path yields nothing") {
        CHECK(Moonraker::uploaded_path(R"({"action":"create_file"})").empty());
    }
    SECTION("a body that is not JSON throws") {
        CHECK_THROWS(Moonraker::uploaded_path("<html>502 Bad Gateway</html>"));
    }
}

TEST_CASE("The Moonraker upload response tells whether Moonraker took the print", "[Moonraker]")
{
    SECTION("an upload with print=true on a ready printer starts the print") {
        // Shape recorded from Moonraker v0.11.0 on the virtual printer rig.
        const std::string body = R"({"action":"create_file","item":{"path":"cube.gcode","root":"gcodes"},)"
                                 R"("print_started":true,"print_queued":false})";
        CHECK(Moonraker::upload_took_print(body));
    }
    SECTION("a job Moonraker queued, as while a power device brings the printer up, counts as taken") {
        CHECK(Moonraker::upload_took_print(
            R"({"action":"create_file","item":{"path":"cube.gcode"},"print_started":false,"print_queued":true})"));
    }
    SECTION("neither flag set leaves the start to the slicer") {
        CHECK_FALSE(Moonraker::upload_took_print(
            R"({"action":"create_file","item":{"path":"cube.gcode"},"print_started":false,"print_queued":false})"));
    }
    SECTION("a server that does not report the flags leaves the start to the slicer") {
        CHECK_FALSE(Moonraker::upload_took_print(R"({"action":"create_file","item":{"path":"cube.gcode"}})"));
    }
    SECTION("an enveloped response is accepted") {
        CHECK(Moonraker::upload_took_print(R"({"result":{"item":{"path":"cube.gcode"},"print_started":true}})"));
    }
    SECTION("a body that is not JSON throws") {
        CHECK_THROWS(Moonraker::upload_took_print("<html>503 Service Unavailable</html>"));
    }
}

TEST_CASE("The Moonraker host takes defaults for print host options a config lacks", "[Moonraker]")
{
    // A full print config carries print_host and printhost_apikey but none of the physical
    // printer's other printhost_* options.
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"print_host", "http://127.0.0.1:7125"}, {"printhost_apikey", "key"}});
    config.erase("printhost_cafile");
    config.erase("printhost_ssl_ignore_revoke");
    REQUIRE_FALSE(config.has("printhost_ssl_ignore_revoke"));

    const Moonraker host(&config);
    CHECK(host.get_host() == "http://127.0.0.1:7125");
    CHECK(host.get_apikey() == "key");
    CHECK(host.get_cafile().empty());
}
