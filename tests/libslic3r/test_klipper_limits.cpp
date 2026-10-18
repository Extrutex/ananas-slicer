#include <catch2/catch_all.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/KlipperLimits.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

namespace {

// Klipper's default extrude-only ratio: a 4 * d^2 cross section over the filament area.
double extrude_ratio(double nozzle_diameter, double filament_diameter)
{
    return 4.0 * nozzle_diameter * nozzle_diameter / (M_PI * std::pow(filament_diameter / 2.0, 2));
}

std::string read_file(const std::string& path)
{
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool has_warning_containing(const KlipperLimitsImport& result, const std::string& needle)
{
    for (const std::string& w : result.warnings)
        if (w.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* minimal_cfg = R"([printer]
kinematics: corexy
max_velocity: 300
max_accel: 3000

[extruder]
nozzle_diameter: 0.4
filament_diameter: 1.75
)";

} // namespace

TEST_CASE("The virtual printer's printer.cfg imports as Klipper resolves it", "[KlipperLimits]")
{
    // tests/virtual-printer/printer.cfg: max_velocity 300, max_accel 7000, minimum_cruise_ratio 0.5,
    // square_corner_velocity 5, Z 15 / 350, 0.4 nozzle on 1.75 filament, pressure_advance 0.04 with a
    // 0.040 s smooth time; no extrude-only limits and no instantaneous_corner_velocity, so Klipper's defaults.
    const std::string cfg = read_file(std::string(TEST_DATA_DIR) + "/../virtual-printer/printer.cfg");
    const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
    const KlipperLimits& l = result.limits;
    CHECK(result.warnings.empty());
    CHECK_THAT(l.max_velocity, WithinRel(300.0, 1e-12));
    CHECK_THAT(l.max_accel, WithinRel(7000.0, 1e-12));
    CHECK_THAT(l.minimum_cruise_ratio, WithinRel(0.5, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(5.0, 1e-12));
    CHECK_THAT(l.max_z_velocity, WithinRel(15.0, 1e-12));
    CHECK_THAT(l.max_z_accel, WithinRel(350.0, 1e-12));
    CHECK_THAT(l.max_extrude_only_velocity, WithinRel(300.0 * extrude_ratio(0.4, 1.75), 1e-12));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(7000.0 * extrude_ratio(0.4, 1.75), 1e-12));
    // The figure the stock-profile retraction acceleration of 5000 mm/s^2 is off from.
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(1862.6, 1e-3));
    CHECK_THAT(l.instantaneous_corner_velocity, WithinRel(1.0, 1e-12));
    CHECK_THAT(l.pressure_advance, WithinRel(0.04, 1e-12));
    CHECK_THAT(l.pressure_advance_smooth_time, WithinRel(0.04, 1e-12));
}

TEST_CASE("Values printer.cfg leaves out take Klipper's defaults", "[KlipperLimits]")
{
    const KlipperLimits l = parse_klipper_printer_cfg(minimal_cfg).limits;
    CHECK_THAT(l.minimum_cruise_ratio, WithinRel(0.5, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(5.0, 1e-12));
    // Z defaults to the XY limits.
    CHECK_THAT(l.max_z_velocity, WithinRel(300.0, 1e-12));
    CHECK_THAT(l.max_z_accel, WithinRel(3000.0, 1e-12));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(3000.0 * extrude_ratio(0.4, 1.75), 1e-12));
    CHECK_THAT(l.instantaneous_corner_velocity, WithinRel(1.0, 1e-12));
    CHECK_THAT(l.pressure_advance_smooth_time, WithinRel(0.04, 1e-12));
    CHECK(l.pressure_advance == 0.0);
}

TEST_CASE("printer.cfg syntax is read the way Klipper's config parser reads it", "[KlipperLimits]")
{
    const std::string cfg = R"(# leading comment
[printer]
Kinematics = cartesian
max_velocity: 250 # inline comment
max_accel: 4000;not a comment without whitespace before it
square_corner_velocity: 8 ; inline comment
max_accel_to_decel: 1000

[gcode_macro PRINT_START]
gcode:
    max_velocity: 1
    G28

[extruder]
nozzle_diameter: 0.6
filament_diameter: 1.75
max_extrude_only_accel: 1500
instantaneous_corner_velocity: 2

#*# <---------------------- SAVE_CONFIG ---------------------->
#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.
#*#
#*# [printer]
#*# max_velocity = 280
)";
    REQUIRE_THROWS_AS(parse_klipper_printer_cfg(cfg), RuntimeError);

    // The same file with a well-formed max_accel.
    std::string fixed = cfg;
    fixed.replace(fixed.find("4000;not"), std::string("4000;not a comment without whitespace before it").size(), "4000");
    const KlipperLimitsImport result = parse_klipper_printer_cfg(fixed);
    const KlipperLimits& l = result.limits;
    // SAVE_CONFIG overrides the file, the macro body is not configuration, option names are case-insensitive.
    CHECK_THAT(l.max_velocity, WithinRel(280.0, 1e-12));
    CHECK_THAT(l.max_accel, WithinRel(4000.0, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(8.0, 1e-12));
    // Deprecated max_accel_to_decel: 1 - 1000 / 4000.
    CHECK_THAT(l.minimum_cruise_ratio, WithinRel(0.75, 1e-12));
    CHECK(has_warning_containing(result, "max_accel_to_decel"));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(1500.0, 1e-12));
    CHECK_THAT(l.max_extrude_only_velocity, WithinRel(280.0 * extrude_ratio(0.6, 1.75), 1e-12));
    CHECK_THAT(l.instantaneous_corner_velocity, WithinRel(2.0, 1e-12));
}

TEST_CASE("printer.cfg import refuses what Klipper refuses", "[KlipperLimits]")
{
    SECTION("no [printer] section") {
        REQUIRE_THROWS_WITH(parse_klipper_printer_cfg("[extruder]\nnozzle_diameter: 0.4\nfilament_diameter: 1.75\n"),
                            ContainsSubstring("[printer]"));
    }
    SECTION("max_z_velocity above max_velocity") {
        const std::string cfg = std::string(minimal_cfg) + "[printer]\nmax_z_velocity: 400\n";
        REQUIRE_THROWS_WITH(parse_klipper_printer_cfg(cfg), ContainsSubstring("max_z_velocity"));
    }
    SECTION("minimum_cruise_ratio of 1") {
        const std::string cfg = std::string(minimal_cfg) + "[printer]\nminimum_cruise_ratio: 1\n";
        REQUIRE_THROWS_WITH(parse_klipper_printer_cfg(cfg), ContainsSubstring("minimum_cruise_ratio"));
    }
    SECTION("a value that is not a number") {
        const std::string cfg = std::string(minimal_cfg) + "[printer]\nsquare_corner_velocity: fast\n";
        REQUIRE_THROWS_WITH(parse_klipper_printer_cfg(cfg), ContainsSubstring("square_corner_velocity"));
    }
}

TEST_CASE("printer.cfg import reports what it cannot model or follow", "[KlipperLimits]")
{
    const std::string cfg = std::string("[include mainsail.cfg]\n") + minimal_cfg + "[printer]\nkinematics: delta\n";
    const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
    CHECK(has_warning_containing(result, "include mainsail.cfg"));
    CHECK(has_warning_containing(result, "delta"));
}

TEST_CASE("Moonraker's toolhead and configfile answer imports the printer.cfg values", "[KlipperLimits]")
{
    // configfile.settings carries printer.cfg with Klipper's defaults filled in; the live toolhead
    // still has an ACCEL from the previous job.
    const std::string json = R"({"result": {"eventtime": 1234.5, "status": {
        "toolhead": {"max_velocity": 300.0, "max_accel": 2500.0, "minimum_cruise_ratio": 0.5, "square_corner_velocity": 5.0},
        "configfile": {"settings": {
            "printer": {"kinematics": "corexy", "max_velocity": 300.0, "max_accel": 7000.0, "minimum_cruise_ratio": 0.5,
                        "square_corner_velocity": 5.0, "max_z_velocity": 15.0, "max_z_accel": 350.0},
            "extruder": {"nozzle_diameter": 0.4, "filament_diameter": 1.75, "max_extrude_only_velocity": 79.8,
                         "max_extrude_only_accel": 1862.6, "instantaneous_corner_velocity": 1.0,
                         "pressure_advance": 0.04, "pressure_advance_smooth_time": 0.04}}}}}})";
    const KlipperLimitsImport result = parse_moonraker_toolhead_query(json);
    const KlipperLimits& l = result.limits;
    CHECK_THAT(l.max_accel, WithinRel(7000.0, 1e-12));
    CHECK_THAT(l.max_z_velocity, WithinRel(15.0, 1e-12));
    CHECK_THAT(l.max_extrude_only_velocity, WithinRel(79.8, 1e-12));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(1862.6, 1e-12));
    CHECK(has_warning_containing(result, "toolhead.max_accel"));
    REQUIRE(result.warnings.size() == 1);
}

TEST_CASE("Moonraker import fails clean on an unusable answer", "[KlipperLimits]")
{
    REQUIRE_THROWS_AS(parse_moonraker_toolhead_query("{not json"), RuntimeError);
    REQUIRE_THROWS_WITH(parse_moonraker_toolhead_query(R"({"result": {"status": {"toolhead": {"max_accel": 7000}}}})"),
                        ContainsSubstring("configfile.settings"));
    // Without the JSON-RPC wrapper the status object is accepted as well.
    REQUIRE_THROWS_WITH(parse_moonraker_toolhead_query(R"({"status": {"configfile": {"settings": {}}}})"),
                        ContainsSubstring("[printer]"));
}

TEST_CASE("Without an imported snapshot the limits come from the machine limits and Klipper's defaults",
          "[KlipperLimits]")
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("machine_max_speed_x", new ConfigOptionFloats({ 400.0, 200.0 }));
    config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({ 6000.0, 1000.0 }));
    config.set_key_value("machine_max_jerk_x", new ConfigOptionFloats({ 7.0, 5.0 }));
    config.set_key_value("machine_max_speed_z", new ConfigOptionFloats({ 500.0, 12.0 }));
    config.set_key_value("machine_max_acceleration_z", new ConfigOptionFloats({ 300.0, 200.0 }));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75 }));

    const KlipperLimits l = klipper_limits_from_config(config);
    CHECK_THAT(l.max_velocity, WithinRel(400.0, 1e-12));
    CHECK_THAT(l.max_accel, WithinRel(6000.0, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(7.0, 1e-12));
    CHECK_THAT(l.minimum_cruise_ratio, WithinRel(0.5, 1e-12));
    // Klipper caps Z at the XY velocity.
    CHECK_THAT(l.max_z_velocity, WithinRel(400.0, 1e-12));
    CHECK_THAT(l.max_z_accel, WithinRel(300.0, 1e-12));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(6000.0 * extrude_ratio(0.4, 1.75), 1e-12));
}

TEST_CASE("An imported snapshot round-trips through the printer config with its provenance", "[KlipperLimits]")
{
    const KlipperLimits imported = parse_klipper_printer_cfg(minimal_cfg).limits;
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    store_klipper_limits(imported, KlipperLimitsSource::PrinterCfg, "2026-10-18", config);

    // Serialized and read back the way a preset or a G-code config block carries it.
    DynamicPrintConfig loaded;
    loaded.apply(FullPrintConfig::defaults());
    for (const std::string key :
         { "klipper_limits_source", "klipper_limits_imported_at", "klipper_max_velocity", "klipper_max_accel",
           "klipper_minimum_cruise_ratio", "klipper_square_corner_velocity", "klipper_max_z_velocity", "klipper_max_z_accel",
           "klipper_max_extrude_only_velocity", "klipper_max_extrude_only_accel", "klipper_instantaneous_corner_velocity",
           "klipper_pressure_advance", "klipper_pressure_advance_smooth_time" }) {
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
        loaded.set_deserialize(key, config.opt_serialize(key), ctx);
    }
    CHECK(config.opt_serialize("klipper_limits_source") == "printer_cfg");
    CHECK(loaded.opt_string("klipper_limits_imported_at") == "2026-10-18");

    const KlipperLimits l = klipper_limits_from_config(config);
    CHECK_THAT(l.max_velocity, WithinRel(imported.max_velocity, 1e-12));
    CHECK_THAT(l.max_accel, WithinRel(imported.max_accel, 1e-12));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(imported.max_extrude_only_accel, 1e-12));
    const KlipperLimits from_loaded = klipper_limits_from_config(loaded);
    // Config serialization keeps six significant digits.
    CHECK_THAT(from_loaded.max_extrude_only_accel, WithinRel(imported.max_extrude_only_accel, 1e-5));

    REQUIRE_THROWS_AS(store_klipper_limits(imported, KlipperLimitsSource::None, "", config), InvalidArgument);
}

TEST_CASE("The Klipper time model is the default for new configs and serializes by name", "[KlipperLimits]")
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    CHECK(config.opt_serialize("time_model") == "klipper_v013");
    CHECK(config.opt_serialize("klipper_limits_source") == "none");
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
    config.set_deserialize("time_model", "legacy", ctx);
    CHECK(config.opt_serialize("time_model") == "legacy");
}
