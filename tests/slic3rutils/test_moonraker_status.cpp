#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/MoonrakerStatus.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("A printing Moonraker reply fills state, file, progress, layers and temperatures", "[MoonrakerStatus]")
{
    const auto reply = nlohmann::json::parse(R"({"result":{"eventtime":1.0,"status":{
        "webhooks":{"state":"ready","state_message":"Printer is ready"},
        "print_stats":{"state":"printing","filename":"bracket.gcode","print_duration":1234.5,
                       "info":{"current_layer":17,"total_layer":120}},
        "display_status":{"progress":0.41,"message":"Layer 17"},
        "virtual_sdcard":{"progress":0.42},
        "extruder":{"temperature":214.6,"target":215.0},
        "heater_bed":{"temperature":59.8,"target":60.0}}}})");
    MoonrakerPrinterStatus s;
    REQUIRE(MoonrakerStatusParser::apply(reply, s));
    CHECK(s.klippy_state == "ready");
    CHECK(s.print_state == "printing");
    CHECK(s.filename == "bracket.gcode");
    CHECK(s.message == "Layer 17");
    CHECK_THAT(s.progress, WithinAbs(0.42, 1e-9)); // virtual_sdcard wins over display_status
    CHECK_THAT(s.print_duration, WithinAbs(1234.5, 1e-9));
    CHECK(s.current_layer == 17);
    CHECK(s.total_layer == 120);
    CHECK_THAT(s.nozzle_temp, WithinAbs(214.6, 1e-9));
    CHECK_THAT(s.nozzle_target, WithinAbs(215.0, 1e-9));
    CHECK(s.has_bed);
    CHECK_THAT(s.bed_target, WithinAbs(60.0, 1e-9));
}

TEST_CASE("A reply without a heater bed leaves the bed unknown and keeps earlier values", "[MoonrakerStatus]")
{
    MoonrakerPrinterStatus s;
    s.filename = "old.gcode";
    const auto reply = nlohmann::json::parse(R"({"result":{"status":{"extruder":{"temperature":25.0,"target":0.0}}}})");
    REQUIRE(MoonrakerStatusParser::apply(reply, s));
    CHECK_FALSE(s.has_bed);
    CHECK(s.filename == "old.gcode");
    CHECK(s.current_layer == -1);
}

TEST_CASE("While Klipper is not ready the strip message is Klipper's state message", "[MoonrakerStatus]")
{
    const auto reply = nlohmann::json::parse(R"({"result":{"status":{
        "webhooks":{"state":"shutdown","state_message":"MCU 'mcu' shutdown: Timer too close"},
        "display_status":{"message":"stale"}}}})");
    MoonrakerPrinterStatus s;
    REQUIRE(MoonrakerStatusParser::apply(reply, s));
    CHECK(s.klippy_state == "shutdown");
    CHECK(s.message == "MCU 'mcu' shutdown: Timer too close");
}

TEST_CASE("A reply that is not an objects query is rejected", "[MoonrakerStatus]")
{
    MoonrakerPrinterStatus s;
    CHECK_FALSE(MoonrakerStatusParser::apply(nlohmann::json::parse(R"({"error":{"code":404}})"), s));
    CHECK_FALSE(MoonrakerStatusParser::apply(nlohmann::json::parse(R"({"result":{"eventtime":1.0}})"), s));
}

TEST_CASE("Two statuses compare equal only when every reported field matches", "[MoonrakerStatus]")
{
    MoonrakerPrinterStatus a, b;
    CHECK(a == b);
    b.nozzle_temp = 1.0;
    CHECK(a != b);
}
