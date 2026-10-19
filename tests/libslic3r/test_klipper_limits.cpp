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
    // Its only [include] is ananas_slicer_state.cfg, which holds macros and no limits; the import reports it
    // as not followed and nothing else.
    const std::string cfg = read_file(std::string(TEST_DATA_DIR) + "/../virtual-printer/printer.cfg");
    const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
    const KlipperLimits& l = result.limits;
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings.front().find("[include /ananas-slicer/ananas_slicer_state.cfg]") != std::string::npos);
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
#*# max_z_velocity = 12
)";
    REQUIRE_THROWS_AS(parse_klipper_printer_cfg(cfg), RuntimeError);

    // The same file with a well-formed max_accel.
    std::string fixed = cfg;
    fixed.replace(fixed.find("4000;not"), std::string("4000;not a comment without whitespace before it").size(), "4000");
    const KlipperLimitsImport result = parse_klipper_printer_cfg(fixed);
    const KlipperLimits& l = result.limits;
    // The macro body is not configuration, option names are case-insensitive. An option set both above
    // and in the SAVE_CONFIG block takes the value above it (configfile.py _strip_duplicates; Klipper
    // v0.13.0 reads max_velocity 250 and max_z_velocity 12 from this file), the block adds the rest.
    CHECK_THAT(l.max_velocity, WithinRel(250.0, 1e-12));
    CHECK_THAT(l.max_z_velocity, WithinRel(12.0, 1e-12));
    CHECK(has_warning_containing(result, "[printer] max_velocity = 280 in the SAVE_CONFIG block is ignored"));
    CHECK_THAT(l.max_accel, WithinRel(4000.0, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(8.0, 1e-12));
    // Deprecated max_accel_to_decel: 1 - 1000 / 4000.
    CHECK_THAT(l.minimum_cruise_ratio, WithinRel(0.75, 1e-12));
    CHECK(has_warning_containing(result, "max_accel_to_decel"));
    CHECK_THAT(l.max_extrude_only_accel, WithinRel(1500.0, 1e-12));
    CHECK_THAT(l.max_extrude_only_velocity, WithinRel(250.0 * extrude_ratio(0.6, 1.75), 1e-12));
    CHECK_THAT(l.instantaneous_corner_velocity, WithinRel(2.0, 1e-12));
}

TEST_CASE("The SAVE_CONFIG block is read only where Klipper reads it", "[KlipperLimits]")
{
    // Expected values from Klipper v0.13.0's ConfigAutoSave.load_main_config on the same text.
    const std::string header = "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
                               "#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.\n"
                               "#*#\n";
    const std::string block  = "#*# [printer]\n#*# max_velocity = 280\n#*# max_accel = 9000\n";
    const std::string base   = std::string(minimal_cfg);

    SECTION("well-formed, CRLF line endings")
    {
        std::string cfg = base + "\n" + header + block;
        for (size_t pos = 0; (pos = cfg.find('\n', pos)) != std::string::npos; pos += 2)
            cfg.insert(pos, "\r");
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_velocity, WithinRel(300.0, 1e-12));
        CHECK_THAT(result.limits.max_accel, WithinRel(3000.0, 1e-12));
        CHECK(has_warning_containing(result, "max_accel = 9000 in the SAVE_CONFIG block is ignored"));
    }
    SECTION("the block adds options the file does not set")
    {
        const std::string cfg = "[printer]\nkinematics: corexy\nmax_accel: 3000\n[extruder]\nnozzle_diameter: 0.4\n"
                                "filament_diameter: 1.75\n" + header + "#*# [printer]\n#*# max_velocity = 280\n";
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_velocity, WithinRel(280.0, 1e-12));
        CHECK(result.warnings.empty());
    }
    SECTION("a \"#*# \" line above the header makes Klipper ignore the block")
    {
        const std::string cfg = "[printer]\nkinematics: corexy\nmax_velocity: 300\n#*# stray\nmax_accel: 3000\n"
                                "[extruder]\nnozzle_diameter: 0.4\nfilament_diameter: 1.75\n" + header +
                                "#*# [printer]\n#*# max_z_velocity = 12\n";
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_z_velocity, WithinRel(300.0, 1e-12));
        CHECK(has_warning_containing(result, "SAVE_CONFIG block is not read"));
    }
    SECTION("an edited line below the header makes Klipper ignore the block, the line counts")
    {
        const std::string cfg = "[extruder]\nnozzle_diameter: 0.4\nfilament_diameter: 1.75\n[printer]\nkinematics: corexy\n"
                                "max_velocity: 300\nmax_accel: 3000\n" + header +
                                "#*# [printer]\n#*# max_z_velocity = 12\nmax_accel: 5000\n";
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_z_velocity, WithinRel(300.0, 1e-12));
        CHECK_THAT(result.limits.max_accel, WithinRel(5000.0, 1e-12));
        CHECK(has_warning_containing(result, "edited below its header"));
    }
    SECTION("\"#*#\" lines without the header are comments")
    {
        const KlipperLimitsImport result = parse_klipper_printer_cfg(base + block);
        CHECK_THAT(result.limits.max_velocity, WithinRel(300.0, 1e-12));
        CHECK_THAT(result.limits.max_accel, WithinRel(3000.0, 1e-12));
    }
    SECTION("a multi-line value in the block does not leak options")
    {
        const std::string cfg = "[printer]\nkinematics: corexy\nmax_accel: 3000\n[extruder]\nnozzle_diameter: 0.4\n"
                                "filament_diameter: 1.75\n" + header +
                                "#*# [gcode_macro X]\n#*# gcode =\n#*# \tmax_accel: 1\n#*# [printer]\n#*# max_velocity = 280\n";
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_velocity, WithinRel(280.0, 1e-12));
        CHECK_THAT(result.limits.max_accel, WithinRel(3000.0, 1e-12));
    }
    SECTION("an indented option in the block is not commented out and overrides the file")
    {
        // _strip_duplicates treats every indented line as a continuation: it comments it out only
        // after a duplicate option. Klipper then reads the indented max_velocity as an option and
        // applies it over the regular one.
        const std::string cfg = base + header + "#*# [printer]\n#*#  max_velocity = 280\n";
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_velocity, WithinRel(280.0, 1e-12));
        CHECK(has_warning_containing(result, "overrides"));
    }
    SECTION("an indented line after a duplicate option is commented out with it")
    {
        const std::string cfg = base + header + "#*# [printer]\n#*# max_accel = 5000\n#*#  max_velocity = 280\n";
        const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
        CHECK_THAT(result.limits.max_velocity, WithinRel(300.0, 1e-12));
        CHECK_THAT(result.limits.max_accel, WithinRel(3000.0, 1e-12));
    }
    SECTION("an indented line after an added option continues that option")
    {
        const std::string cfg = base + header + "#*# [printer]\n#*# square_corner_velocity = 7\n#*#  max_velocity = 280\n";
        REQUIRE_THROWS_WITH(parse_klipper_printer_cfg(cfg), ContainsSubstring("square_corner_velocity"));
    }
}

TEST_CASE("printer.cfg comments end where Klipper ends them", "[KlipperLimits]")
{
    // configfile.py cuts every line at its first '#', whitespace before it or not. ';' is left to
    // configparser, where it starts a comment only at the start of a line or after whitespace.
    const std::string cfg = R"([printer]#comment right after the header
kinematics: corexy#comment
max_velocity: 300#comment
max_accel: 7000#comment;more
square_corner_velocity: 6 ;comment
   ; indented comment line

[extruder]
nozzle_diameter: 0.4#comment
filament_diameter: 1.75
pressure_advance: 0.05#comment
)";
    const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
    const KlipperLimits& l = result.limits;
    CHECK(result.warnings.empty());
    CHECK_THAT(l.max_velocity, WithinRel(300.0, 1e-12));
    CHECK_THAT(l.max_accel, WithinRel(7000.0, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(6.0, 1e-12));
    CHECK_THAT(l.pressure_advance, WithinRel(0.05, 1e-12));

    // A ';' right after the value is part of it, and Klipper refuses the number.
    REQUIRE_THROWS_WITH(parse_klipper_printer_cfg(std::string(minimal_cfg) + "[printer]\nmax_accel: 7000;comment\n"),
                        ContainsSubstring("max_accel"));
}

TEST_CASE("printer.cfg indentation continues a value only where configparser continues it", "[KlipperLimits]")
{
    // configparser continues an option's value with lines indented deeper than the option's own
    // line. Options indented alike are siblings, and blank or comment lines inside a value do not
    // end it.
    const std::string cfg = R"([printer]
  kinematics: corexy
  max_velocity: 300
  max_accel: 7000
  square_corner_velocity: 6

[gcode_macro PRINT_START]
gcode:
    G28

    # A comment line inside the macro body.
    max_accel: 1
  ; and another, indented less
    max_velocity: 1

[quad_gantry_level]
gantry_corners:
    -60,-10
    410,420
max_adjust: 10

[extruder]
	nozzle_diameter: 0.4
	filament_diameter: 1.75
	pressure_advance: 0.05
)";
    const KlipperLimitsImport result = parse_klipper_printer_cfg(cfg);
    const KlipperLimits& l = result.limits;
    CHECK(result.warnings.empty());
    CHECK_THAT(l.max_velocity, WithinRel(300.0, 1e-12));
    CHECK_THAT(l.max_accel, WithinRel(7000.0, 1e-12));
    CHECK_THAT(l.square_corner_velocity, WithinRel(6.0, 1e-12));
    CHECK_THAT(l.pressure_advance, WithinRel(0.05, 1e-12));
}

TEST_CASE("printer.cfg continuation lines are part of the value", "[KlipperLimits]")
{
    // configparser joins an option's continuation lines into its value with newlines, and Klipper
    // converts the whole value. Klipper v0.13.0 reads max_accel "\n3000" as 3000 and refuses
    // "3000\n9000" (could not convert string to float).
    const std::string head = "[extruder]\nnozzle_diameter: 0.4\nfilament_diameter: 1.75\n"
                             "[printer]\nkinematics: corexy\nmax_velocity: 300\n";
    SECTION("the value on the next line")
    {
        const KlipperLimitsImport result = parse_klipper_printer_cfg(head + "max_accel:\n  3000\n");
        CHECK_THAT(result.limits.max_accel, WithinRel(3000.0, 1e-12));
    }
    SECTION("blank and comment lines before it")
    {
        const KlipperLimitsImport result = parse_klipper_printer_cfg(head + "max_accel:\n\n# comment\n  3000\n");
        CHECK_THAT(result.limits.max_accel, WithinRel(3000.0, 1e-12));
    }
    SECTION("two numbers are not one")
    {
        REQUIRE_THROWS_WITH(parse_klipper_printer_cfg(head + "max_accel: 3000\n  9000\n"),
                            ContainsSubstring("max_accel"));
    }
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
