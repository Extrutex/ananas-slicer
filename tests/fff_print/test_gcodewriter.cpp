#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ModelArrange.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Arrange on a finite bed, not an unbounded InfiniteBed: the latter places items
// near INT64_MIN/4 (~2.3e18), which reaches ClipperLib's coordinate limit and throws
// "Coordinate outside allowed range" on Windows/arm64. A 500x500 bed keeps coordinates
// small while still covering large printers.
static void arrange_objects_on_test_bed(Model &model, const DynamicPrintConfig &config)
{
    const BoundingBox bed{Point::new_scale(0., 0.), Point::new_scale(500., 500.)};
    arrange_objects(model, bed, ArrangeParams{scaled(min_object_distance(config))});
}

SCENARIO("set_speed emits values with fixed-point output.", "[GCodeWriter]") {

    GIVEN("GCodeWriter instance") {
        GCodeWriter writer;
        WHEN("set_speed is called to set speed to 99999.123") {
            THEN("Output string is G1 F99999.123") {
                REQUIRE_THAT(writer.set_speed(99999.123), Catch::Matchers::Equals("G1 F99999.123\n"));
            }
        }
        WHEN("set_speed is called to set speed to 1") {
            THEN("Output string is G1 F1") {
                REQUIRE_THAT(writer.set_speed(1.0), Catch::Matchers::Equals("G1 F1\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200022") {
            THEN("Output string is G1 F203.2") {
                REQUIRE_THAT(writer.set_speed(203.200022), Catch::Matchers::Equals("G1 F203.2\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200522") {
            THEN("Output string is G1 F203.201") {
                REQUIRE_THAT(writer.set_speed(203.200522), Catch::Matchers::Equals("G1 F203.201\n"));
            }
        }
    }
}

SCENARIO("z_hop lifts the nozzle when a lift is requested", "[GCodeWriter]") {
    GIVEN("A writer with the nozzle parked at Z = 10") {
        GCodeWriter writer;
        std::vector<unsigned int> extruder_ids { 0 };
        writer.set_extruders(extruder_ids);
        writer.set_extruder(0);
        writer.travel_to_z(10.0);

        WHEN("z_hop is 1 and an eager lift is requested") {
            writer.config.z_hop.values = { 1.0 };
            std::string gcode = writer.eager_lift(LiftType::NormalLift);
            THEN("a Z move up by z_hop is emitted") {
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("Z11"));
            }
        }
        WHEN("z_hop is 0") {
            writer.config.z_hop.values = { 0.0 };
            std::string gcode = writer.eager_lift(LiftType::NormalLift);
            THEN("no lift is emitted") {
                REQUIRE(gcode.empty());
            }
        }
    }
}

SCENARIO("Origin manipulation", "[GCodeWriter]") {
	Slic3r::GCode gcodegen;
	WHEN("set_origin to (10,0)") {
    	gcodegen.set_origin(Vec2d(10,0));
    	REQUIRE(gcodegen.origin() == Vec2d(10, 0));
    }
	WHEN("set_origin to (10,0) and translate by (5, 5)") {
		gcodegen.set_origin(Vec2d(10,0));
		gcodegen.set_origin(gcodegen.origin() + Vec2d(5, 5));
		THEN("origin returns reference to point") {
    		REQUIRE(gcodegen.origin() == Vec2d(15,5));
    	}
    }
}

static std::string slice_cube(std::initializer_list<ConfigBase::SetDeserializeItem> config_items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict(config_items);
    return Slic3r::Test::slice({ cube(20) }, config);
}

// Klipper keeps its machine limits in printer.cfg; the slicer never writes Marlin's
// M201/M203/M204/M205 envelope, whatever emit_machine_limits_to_gcode says.
TEST_CASE("No machine envelope is written for Klipper", "[GCodeWriter][Klipper]")
{
    const std::string gcode = slice_cube({
        { "emit_machine_limits_to_gcode", "1" },
        { "machine_start_gcode",          "" },
        { "machine_max_acceleration_extruding", "1200,1300" },
        { "machine_max_junction_deviation",     "0.02,0.03" },
    });
    std::istringstream stream(gcode);
    std::string line;
    while (std::getline(stream, line)) {
        CAPTURE(line);
        for (const char *command : { "M201 ", "M203 ", "M204 ", "M205 ", "M566 " })
            CHECK(line.rfind(command, 0) != 0);
    }
}

// EXTRUDER_LIMIT (GCodeWriter.cpp) clamps to the active extruder's own limit and falls back to
// the largest limit of all extruders while no filament is active.
TEST_CASE("Acceleration and corner velocity are clamped per extruder", "[GCodeWriter][Klipper]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Stride-2 (normal, silent) per extruder: extruder 0 low, extruder 1 high. The ACCEL cap is
    // the smallest of the extruding, X and Y limits.
    config.set_key_value("nozzle_diameter",                    new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("filament_diameter",                  new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_map",                       new ConfigOptionInts({1, 2}));
    config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({500, 0, 2000, 0}));
    config.set_key_value("machine_max_acceleration_x",         new ConfigOptionFloats({800, 0, 2500, 0}));
    config.set_key_value("machine_max_acceleration_y",         new ConfigOptionFloats({800, 0, 2500, 0}));
    config.set_key_value("machine_max_jerk_x",                 new ConfigOptionFloats({5, 0, 15, 0}));
    config.set_key_value("machine_max_jerk_y",                 new ConfigOptionFloats({6, 0, 16, 0}));
    PrintConfig print_config;
    print_config.apply(config, true);

    GCodeWriter writer;
    writer.apply_print_config(print_config);
    writer.set_extruders({0, 1});

    SECTION("no active filament: the largest limit applies") {
        CHECK_THAT(writer.set_print_acceleration(5000), Catch::Matchers::StartsWith("SET_VELOCITY_LIMIT ACCEL=2000"));
        CHECK_THAT(writer.set_jerk_xy(20.), Catch::Matchers::StartsWith("SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=15"));
    }
    SECTION("extruder 0 is clamped to its own limits") {
        writer.toolchange(0, 0);
        CHECK_THAT(writer.set_accel_and_jerk(1500, 10.), Catch::Matchers::StartsWith("SET_VELOCITY_LIMIT ACCEL=500 SQUARE_CORNER_VELOCITY=5"));
    }
    SECTION("extruder 1 is not clamped to extruder 0's limits") {
        writer.toolchange(1, 1);
        CHECK_THAT(writer.set_accel_and_jerk(1500, 10.), Catch::Matchers::StartsWith("SET_VELOCITY_LIMIT ACCEL=1500 SQUARE_CORNER_VELOCITY=10"));
    }
}

SCENARIO("Extruder reads the injected config column", "[GCodeWriter][H2C]") {
    GIVEN("A writer whose per-variant arrays hold three columns for two filaments") {
        GCodeWriter writer;
        // Column layout after a migrating regroup: filament 0 -> column 0, filament 1 ->
        // columns 1 (its first variant) and 2 (its second variant).
        writer.config.retraction_length.values   = {0.8, 0.5, 1.2};
        writer.config.z_hop.values               = {0.4, 0.6, 0.9};
        writer.config.retraction_speed.values    = {30., 40., 50.};
        writer.config.filament_flow_ratio.values = {0.98, 1.0, 1.02};
        // Filament-indexed arrays keep one entry per filament.
        writer.config.filament_diameter.values   = {1.75, 1.75};
        writer.set_extruders({0, 1});
        writer.toolchange(1, 1);
        Extruder *fil = writer.filament();
        REQUIRE(fil != nullptr);
        REQUIRE(fil->id() == 1);
        const double crossection = 1.75 * 1.75 * 0.25 * PI;

        WHEN("no column has been injected") {
            THEN("the getters read the filament id's column") {
                REQUIRE(fil->config_index() == 1);
                REQUIRE_THAT(fil->retraction_length(), Catch::Matchers::WithinAbs(0.5, 1e-9));
                REQUIRE_THAT(fil->retract_lift(), Catch::Matchers::WithinAbs(0.6, 1e-9));
                REQUIRE(fil->retract_speed() == 40);
                REQUIRE_THAT(fil->e_per_mm3(), Catch::Matchers::WithinRel(1.0 / crossection, 1e-9));
            }
        }
        WHEN("the second variant column is injected") {
            fil->set_config_index(2);
            THEN("the getters follow the column and the flow cache is rescaled") {
                REQUIRE(fil->config_index() == 2);
                REQUIRE_THAT(fil->retraction_length(), Catch::Matchers::WithinAbs(1.2, 1e-9));
                REQUIRE_THAT(fil->retract_lift(), Catch::Matchers::WithinAbs(0.9, 1e-9));
                REQUIRE(fil->retract_speed() == 50);
                REQUIRE_THAT(fil->e_per_mm3(), Catch::Matchers::WithinRel(1.02 / crossection, 1e-9));
            }
            THEN("filament-indexed reads keep using the filament id") {
                REQUIRE_THAT(fil->filament_diameter(), Catch::Matchers::WithinAbs(1.75, 1e-9));
            }
        }
        WHEN("a negative index is injected") {
            fil->set_config_index(2);
            fil->set_config_index(-1);
            THEN("resolution resets to the filament id") {
                REQUIRE(fil->config_index() == 1);
                REQUIRE_THAT(fil->retraction_length(), Catch::Matchers::WithinAbs(0.5, 1e-9));
                REQUIRE_THAT(fil->e_per_mm3(), Catch::Matchers::WithinRel(1.0 / crossection, 1e-9));
            }
        }
    }
}

// Numeric argument of every line starting with `prefix`, in file order.
static std::vector<int> collect_line_args(const std::string &gcode, const std::string &prefix)
{
    std::vector<int> values;
    std::istringstream stream(gcode);
    std::string line;
    while (std::getline(stream, line))
        if (line.compare(0, prefix.size(), prefix) == 0)
            values.push_back(std::atoi(line.c_str() + int(prefix.size())));
    return values;
}

static int count_lines_with_prefix(const std::string &gcode, const std::string &prefix)
{
    return (int) collect_line_args(gcode, prefix).size();
}

// A toolchange ordinal sequence is healthy when it advances by exactly one per
// change block; a change-less prime-tower visit must not consume an ordinal.
static bool ordinals_consecutive(const std::vector<int> &values)
{
    for (size_t i = 1; i < values.size(); ++i)
        if (values[i] != values[i - 1] + 1)
            return false;
    return true;
}

SCENARIO("Toolchange emission and prefix", "[GCodeWriter][H2C]") {
    GIVEN("A dual-extruder writer with two filaments") {
        GCodeWriter writer;
        writer.config.filament_diameter.values = {1.75, 1.75};
        writer.set_extruders({0, 1});

        WHEN("the filament changes") {
            THEN("toolchange emits the plain T command") {
                REQUIRE_THAT(writer.toolchange_prefix(), Catch::Matchers::Equals("T"));
                const std::string gcode = writer.toolchange(1, 0);
                REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("T1"));
                REQUIRE_THAT(gcode, !Catch::Matchers::ContainsSubstring("M1020"));
            }
        }
        WHEN("manual filament change is on") {
            writer.config.manual_filament_change.value = true;
            THEN("the toolchange is the manual tag line") {
                REQUIRE_THAT(writer.toolchange_prefix(), Catch::Matchers::StartsWith(";"));
                const std::string gcode = writer.toolchange(1, 0);
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring(writer.toolchange_prefix() + "1"));
            }
        }
    }
}

// Shared dual-extruder printer config for the toolchange-count scenarios below.
static DynamicPrintConfig dual_extruder_toolchange_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("emit_machine_limits_to_gcode", new ConfigOptionBool(false));
    config.set_key_value("machine_start_gcode",        new ConfigOptionString(""));
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_line_width",   new ConfigOptionFloatOrPercent(0, false));
    config.set_key_value("z_hop",                      new ConfigOptionFloats({0., 0.}));
    // The change block carries both a real toolchange command and the ordinal
    // placeholder the stock profiles feed to the firmware.
    config.set_key_value("change_filament_gcode",
                         new ConfigOptionString("T[next_filament_id]\nM620 O{toolchange_count + 1}\n"));

    // 2 extruders, one filament each (manual map so nothing regroups them).
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 140, 140, 0}));
    // Inside the 200x200 test bed; the default y, 220, is not, and generation rejects that.
    config.set_key_value("wipe_tower_x",         new ConfigOptionFloats({50.}));
    config.set_key_value("wipe_tower_y",         new ConfigOptionFloats({50.}));
    return config;
}

SCENARIO("Change blocks carry consecutive toolchange ordinals without a duplicate command", "[GCodeWriter][H2C]") {
    GIVEN("Two sequentially printed objects on different extruders") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

        Model model;
        auto *obj1 = model.add_object();
        obj1->add_volume(cube(20));
        obj1->add_instance();
        auto *obj2 = model.add_object();
        obj2->add_volume(cube(20));
        obj2->add_instance();
        obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

        auto slice_to_gcode = [&]() {
            Print print;
            arrange_objects_on_test_bed(model, config);
            for (auto *mo : model.objects) {
                mo->ensure_on_bed();
                print.auto_assign_extruders(mo);
            }
            print.apply(model, config);
            print.validate();
            print.set_status_silent();
            print.process();
            return Slic3r::Test::gcode(print);
        };

        WHEN("the change block already changes the tool") {
            const std::string gcode = slice_to_gcode();
            const std::vector<int> ordinals = collect_line_args(gcode, "M620 O");
            THEN("each change block advances the ordinal by exactly one, without inflation") {
                REQUIRE(!ordinals.empty());
                REQUIRE(ordinals_consecutive(ordinals));
                REQUIRE(ordinals.front() <= 3);
            }
            THEN("the writer's own command is suppressed as a duplicate") {
                REQUIRE(count_lines_with_prefix(gcode, "T1") == 1);
            }
        }
        WHEN("the change block does not change the tool itself") {
            config.set_key_value("change_filament_gcode",
                                 new ConfigOptionString("M620 O{toolchange_count + 1}\n"));
            const std::string gcode = slice_to_gcode();
            const std::vector<int> ordinals = collect_line_args(gcode, "M620 O");
            THEN("the writer's own toolchange survives") {
                REQUIRE(count_lines_with_prefix(gcode, "T1") == 1);
            }
            THEN("the ordinal sequence stays consecutive") {
                REQUIRE(!ordinals.empty());
                REQUIRE(ordinals_consecutive(ordinals));
                REQUIRE(ordinals.front() <= 3);
            }
        }
    }
}

SCENARIO("Prime-tower visits without a filament change do not advance the toolchange ordinal", "[GCodeWriter][H2C]") {
    GIVEN("A print whose only filament change happens far above the bed") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));

        // Filament 2 is used only above z=6, so every tower layer below it is a
        // change-less visit — the exact geometry that used to inflate the ordinal.
        Model model;
        auto *obj = model.add_object();
        obj->add_volume(cube(10));
        obj->add_instance();
        DynamicPrintConfig range_config;
        range_config.set_key_value("extruder", new ConfigOptionInt(2));
        // Every layer range must carry a layer_height (see layer_height_profile_from_ranges).
        range_config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        obj->layer_config_ranges[{6.0, 10.0}].assign_config(std::move(range_config));

        Print print;
        arrange_objects_on_test_bed(model, config);
        for (auto *mo : model.objects) {
            mo->ensure_on_bed();
            print.auto_assign_extruders(mo);
        }
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
        const std::string gcode = Slic3r::Test::gcode(print);

        WHEN("the print is exported") {
            const std::vector<int> ordinals = collect_line_args(gcode, "M620 O");
            THEN("the prime-tower toolchange path was exercised") {
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("CP TOOLCHANGE START"));
            }
            THEN("dozens of change-less tower layers consume no ordinal") {
                REQUIRE(!ordinals.empty());
                REQUIRE(ordinals_consecutive(ordinals));
                REQUIRE(ordinals.front() <= 3);
            }
            THEN("no duplicate toolchange command follows the change block") {
                REQUIRE(count_lines_with_prefix(gcode, "T1") == 1);
            }
        }
    }
}

// Two 20mm cubes on separate extruders, printed by object so exactly one real
// toolchange fires and drives the change_filament_gcode. Returns the g-code.
static std::string slice_two_object_dual(DynamicPrintConfig &config)
{
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

    Model model;
    auto *obj1 = model.add_object();
    obj1->add_volume(cube(20));
    obj1->add_instance();
    auto *obj2 = model.add_object();
    obj2->add_volume(cube(20));
    obj2->add_instance();
    obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

    Print print;
    arrange_objects_on_test_bed(model, config);
    for (auto *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    return Slic3r::Test::gcode(print);
}

SCENARIO("Toolchange gcode resolves old/new_extruder_variant from printer_extruder_variant", "[GCodeWriter][H2C]")
{
    GIVEN("a dual-extruder print whose change gcode reads the extruder-variant placeholders") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        // A distinctive variant that can only reach the g-code through printer_extruder_variant.
        // Both entries carry it so the assertion is independent of which physical extruder the
        // emitted change routes through.
        config.set_key_value("printer_extruder_variant",
                             new ConfigOptionStrings({"Direct Drive TPU High Flow", "Direct Drive TPU High Flow"}));
        config.set_key_value("change_filament_gcode", new ConfigOptionString(
            "; VARIANT old={old_extruder_variant} new={new_extruder_variant}\nT[next_filament_id]\n"));

        WHEN("the print is sliced") {
            const std::string gcode = slice_two_object_dual(config);
            THEN("both placeholders resolve to the printer_extruder_variant value") {
                // The resolved line is the proof: an unresolved token or a parser throw would
                // prevent this exact line from being emitted. (A negative "{token}" check is
                // unreliable — the g-code's trailing config dump echoes the raw template.)
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring(
                    "; VARIANT old=Direct Drive TPU High Flow new=Direct Drive TPU High Flow"));
            }
        }
    }
}

SCENARIO("Global current-tool placeholders resolve in a context with no local injection", "[GCodeWriter][H2C]")
{
    GIVEN("a dual-extruder print whose before_layer_change_gcode reads the current-tool placeholders") {
        DynamicPrintConfig config = dual_extruder_toolchange_config();
        // before_layer_change is one of the contexts that inject NO current_* into their local config
        // (unlike change_filament / machine_end / layer_change), so these placeholders can only resolve
        // through the GLOBAL parser vars published at each toolchange (and at the initial set_extruder).
        // Pre-fix current_filament_id / current_extruder_id / current_nozzle_id were undefined here and the
        // whole slice threw a PlaceholderParserError — the same failure mode X2D's layer_change hit.
        config.set_key_value("before_layer_change_gcode", new ConfigOptionString(
            "; GVAR fid={current_filament_id} eid={current_extruder_id} nid={current_nozzle_id}\n"));

        WHEN("the print is sliced (obj1 on filament 0, obj2 on filament 1)") {
            std::string gcode;
            REQUIRE_NOTHROW(gcode = slice_two_object_dual(config));
            THEN("all three globals resolve to the CORRECT active-tool values on both sides of the change") {
                // Assert the FULL resolved marker, not just no-throw: obj1 prints on filament 0 (extruder 0,
                // nozzle 0) and obj2 on filament 1 (extruder 1, nozzle 1). Locking every field means a
                // stale or wrong global (e.g. obj2 still reading fid=0, or a mismatched extruder/nozzle id)
                // fails here — this is the value guard that replaces the old "throws on undefined" canary.
                // Only the emitted before_layer_change lines carry resolved values; the trailing config dump
                // keeps the raw "{current_filament_id}" template, so these are unambiguous.
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("; GVAR fid=0 eid=0 nid=0"));
                REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("; GVAR fid=1 eid=1 nid=1"));
            }
        }
    }
}

TEST_CASE("Custom G-code motion limits are restored before generated moves", "[GCodeWriter][Klipper]")
{
    const std::string gcode = slice_cube({
        { "gcode_comments",              "1" },
        { "machine_start_gcode",         "" },
        { "layer_change_gcode",          "SET_VELOCITY_LIMIT ACCEL=5000 SQUARE_CORNER_VELOCITY=5\n" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "initial_layer_line_width",    "0" },
        { "z_hop",                       "0" },
        { "machine_max_acceleration_extruding", "20000,20000" },
        { "machine_max_acceleration_x",  "20000,20000" },
        { "machine_max_acceleration_y",  "20000,20000" },
        { "machine_max_jerk_x",          "20,20" },
        { "machine_max_jerk_y",          "20,20" },
        { "default_acceleration",        "6000" },
        { "initial_layer_acceleration",  "6000" },
        { "outer_wall_acceleration",     "6000" },
        { "inner_wall_acceleration",     "0" },
        { "travel_acceleration",         "6000" },
        { "default_jerk",                "8" },
        { "initial_layer_jerk",          "8" },
        { "outer_wall_jerk",             "8" },
        { "inner_wall_jerk",             "0" },
        { "travel_jerk",                 "8" },
    });

    const size_t custom_gcode_pos = gcode.find("SET_VELOCITY_LIMIT ACCEL=5000 SQUARE_CORNER_VELOCITY=5\n");
    REQUIRE(custom_gcode_pos != std::string::npos);
    REQUIRE(gcode.find("SET_VELOCITY_LIMIT ACCEL=6000 SQUARE_CORNER_VELOCITY=8", custom_gcode_pos) != std::string::npos);
}

TEST_CASE("Klipper acceleration commands preserve the configured cruise ratio", "[GCodeWriter][Klipper]")
{
    const double factor = GENERATE(1.0, 25.0, 50.0, 100.0);
    const bool enabled = GENERATE(false, true);
    const bool combined = GENERATE(false, true);
    GCodeWriter writer;
    writer.config.gcode_flavor.value = gcfKlipper;
    writer.config.accel_to_decel_enable.value = enabled;
    writer.config.accel_to_decel_factor.value = factor;
    const std::string command = combined ? writer.set_accel_and_jerk(1200, 5.0)
                                         : writer.set_print_acceleration(1200);
    REQUIRE_THAT(command, Catch::Matchers::ContainsSubstring("ACCEL=1200"));
    REQUIRE(command.find("ACCEL_TO_DECEL=") == std::string::npos);
    const auto position = command.find("MINIMUM_CRUISE_RATIO=");
    if (enabled) {
        REQUIRE(position != std::string::npos);
        const double ratio = std::stod(command.substr(position + std::string("MINIMUM_CRUISE_RATIO=").size()));
        REQUIRE_THAT(ratio, Catch::Matchers::WithinAbs(1.0 - factor / 100.0, 1e-6));
    } else {
        REQUIRE(position == std::string::npos);
    }
}

TEST_CASE("Klipper prime towers emit the configured minimum cruise ratio", "[GCodeWriter][Klipper][WipeTower]")
{
    DynamicPrintConfig config = dual_extruder_toolchange_config();
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfKlipper));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("wipe_tower_type", new ConfigOptionEnum<WipeTowerType>(WipeTowerType::Type1));
    config.set_key_value("accel_to_decel_enable", new ConfigOptionBool(true));
    config.set_key_value("accel_to_decel_factor", new ConfigOptionPercent(37.));
    Model model;
    auto *object = model.add_object();
    object->add_volume(cube(4));
    object->add_instance();
    DynamicPrintConfig upper_layers;
    upper_layers.set_key_value("extruder", new ConfigOptionInt(2));
    upper_layers.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    object->layer_config_ranges[{2.0, 4.0}].assign_config(std::move(upper_layers));
    Print print;
    arrange_objects_on_test_bed(model, config);
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    const std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(gcode.find("ACCEL_TO_DECEL=") == std::string::npos);
    REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=0.63"));
}

// Replays G10/G11/SET_RETRACTION with the semantics of Klipper's [firmware_retraction]
// (klippy/extras/firmware_retraction.py): G10 and G11 act only on a change of the global
// "retracted" flag, and SET_RETRACTION clears that flag.
struct KlipperFirmwareRetraction
{
    struct Values { double length = 0., speed = 0., extra = 0., unretract_speed = 0.; };

    Values active;
    std::vector<Values> sent;
    // Retraction values in effect at every G10 that moved filament, with the tool active then.
    std::vector<std::pair<int, Values>> retractions;
    int    set_while_retracted = 0;
    int    dropped_unretractions = 0;
    // G10 issued while Klipper is already retracted: it moves no filament.
    int    ignored_retractions = 0;
    double net_e = 0.;

    explicit KlipperFirmwareRetraction(const std::string &gcode)
    {
        bool retracted = false;
        int  tool = 0;
        std::istringstream stream(gcode);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.rfind("SET_RETRACTION ", 0) == 0) {
                auto arg = [&line](const std::string &name) {
                    const auto pos = line.find(" " + name + "=");
                    REQUIRE(pos != std::string::npos);
                    return std::stod(line.substr(pos + name.size() + 2));
                };
                active = { arg("RETRACT_LENGTH"), arg("RETRACT_SPEED"), arg("UNRETRACT_EXTRA_LENGTH"), arg("UNRETRACT_SPEED") };
                sent.push_back(active);
                if (retracted)
                    ++ set_while_retracted;
                retracted = false;
            } else if (line.rfind("G10", 0) == 0) {
                if (! retracted) {
                    net_e -= active.length;
                    retractions.emplace_back(tool, active);
                } else
                    ++ ignored_retractions;
                retracted = true;
            } else if (line.rfind("G11", 0) == 0) {
                if (retracted)
                    net_e += active.length + active.extra;
                else
                    ++ dropped_unretractions;
                retracted = false;
            } else if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1])) {
                tool = std::atoi(line.c_str() + 1);
            }
        }
    }
};

static DynamicPrintConfig firmware_retraction_config(bool firmware_owns, bool shared_nozzle = false)
{
    DynamicPrintConfig config = dual_extruder_toolchange_config();
    if (shared_nozzle) {
        // Both filaments feed one nozzle, as with a filament changer (MMU).
        config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
        config.set_key_value("printer_extruder_id", new ConfigOptionInts({1}));
        config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard"}));
        config.set_key_value("filament_map", new ConfigOptionInts({1, 1}));
    }
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfKlipper));
    config.set_key_value("use_firmware_retraction", new ConfigOptionBool(true));
    config.set_key_value("firmware_owns_retraction", new ConfigOptionBool(firmware_owns));
    config.set_key_value("retraction_length", new ConfigOptionFloats({0.5, 1.2}));
    config.set_key_value("retraction_speed", new ConfigOptionFloats({35., 25.}));
    config.set_key_value("deretraction_speed", new ConfigOptionFloats({0., 20.}));
    config.set_key_value("retract_restart_extra", new ConfigOptionFloats({0., 0.1}));
    config.set_key_value("wipe", new ConfigOptionBools({false, false}));
    // One filament-variant column per filament, so each filament reads its own retraction values.
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_self_index", new ConfigOptionInts({1, 2}));
    return config;
}

// One cube; the layers from 2 mm up print with the second filament when `two_filaments`.
static std::string slice_firmware_retraction(const DynamicPrintConfig &config, bool two_filaments)
{
    Model model;
    auto *object = model.add_object();
    object->add_volume(cube(10));
    object->add_instance();
    if (two_filaments) {
        DynamicPrintConfig upper_layers;
        upper_layers.set_key_value("extruder", new ConfigOptionInt(2));
        upper_layers.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        object->layer_config_ranges[{2.0, 10.0}].assign_config(std::move(upper_layers));
    }
    Print print;
    arrange_objects_on_test_bed(model, config);
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    REQUIRE(print.validate().string.empty());
    print.set_status_silent();
    print.process();
    return Slic3r::Test::gcode(print);
}

TEST_CASE("Firmware retraction keeps printer.cfg values unless the slicer owns them", "[GCodeWriter][Klipper][Retraction]")
{
    const std::string gcode = slice_firmware_retraction(firmware_retraction_config(true), true);
    REQUIRE(gcode.find("\nG10") != std::string::npos);
    REQUIRE(gcode.find("SET_RETRACTION") == std::string::npos);
}

TEST_CASE("Slicer-owned firmware retraction sends each filament's values to Klipper", "[GCodeWriter][Klipper][Retraction]")
{
    // Two filaments on independent nozzles are rejected by validation, see below.
    const auto [two_filaments, shared_nozzle] = GENERATE(std::make_pair(false, false), std::make_pair(false, true),
                                                         std::make_pair(true, true));
    CAPTURE(two_filaments, shared_nozzle);
    const std::string gcode = slice_firmware_retraction(firmware_retraction_config(false, shared_nozzle), two_filaments);
    const KlipperFirmwareRetraction klipper(gcode);

    REQUIRE(! klipper.retractions.empty());
    // The command never lands between a G10 and its G11, where Klipper would drop the G11.
    CHECK(klipper.set_while_retracted == 0);
    CHECK(klipper.dropped_unretractions == 0);
    CHECK(klipper.ignored_retractions == 0);
    // Every G10 runs with the values of the filament that is printing.
    const KlipperFirmwareRetraction::Values expected[] = { {0.5, 35., 0., 35.}, {1.2, 25., 0.1, 20.} };
    for (const auto &[tool, values] : klipper.retractions) {
        CAPTURE(tool);
        REQUIRE((tool == 0 || tool == 1));
        CHECK(values.length == Catch::Approx(expected[tool].length));
        CHECK(values.speed == Catch::Approx(expected[tool].speed));
        CHECK(values.extra == Catch::Approx(expected[tool].extra));
        CHECK(values.unretract_speed == Catch::Approx(expected[tool].unretract_speed));
    }
    // One command per filament activation, none repeated while the values stay the same.
    CHECK(klipper.sent.size() == (two_filaments ? 2 : 1));
    if (two_filaments)
        CHECK(std::any_of(klipper.retractions.begin(), klipper.retractions.end(), [](const auto &r) { return r.first == 1; }));
}

TEST_CASE("Custom G-code keeps the firmware retraction state in step with Klipper", "[GCodeWriter][Klipper][Retraction]")
{
    DynamicPrintConfig config = firmware_retraction_config(false);
    SECTION("start G-code leaves Klipper retracted") {
        config.set_key_value("machine_start_gcode", new ConfigOptionString("G10 ; park retracted\n"));
        const KlipperFirmwareRetraction klipper(slice_firmware_retraction(config, false));
        // SET_RETRACTION would clear the flag and strand the start G-code's retraction in the nozzle.
        CHECK(klipper.set_while_retracted == 0);
        CHECK(klipper.dropped_unretractions == 0);
        // The start G-code retracted with the printer.cfg values; every later G10 with the filament's.
        REQUIRE(klipper.retractions.size() > 1);
        for (size_t i = 1; i < klipper.retractions.size(); ++ i)
            CHECK(klipper.retractions[i].second.length == Catch::Approx(0.5));
    }
    SECTION("layer change G-code sends its own retraction values") {
        config.set_key_value("layer_change_gcode", new ConfigOptionString(
            "SET_RETRACTION RETRACT_LENGTH=3 RETRACT_SPEED=10 UNRETRACT_EXTRA_LENGTH=0 UNRETRACT_SPEED=10\n"));
        const KlipperFirmwareRetraction klipper(slice_firmware_retraction(config, false));
        CHECK(klipper.set_while_retracted == 0);
        CHECK(klipper.dropped_unretractions == 0);
        CHECK(klipper.ignored_retractions == 0);
        // Every G10 after the custom values runs with the filament's values again.
        REQUIRE(! klipper.retractions.empty());
        for (const auto &[tool, values] : klipper.retractions) {
            CHECK(values.length == Catch::Approx(0.5));
            CHECK(values.speed == Catch::Approx(35.));
        }
    }
}

TEST_CASE("Firmware retraction on independent nozzles is rejected or warned about", "[GCodeWriter][Klipper][Retraction]")
{
    Model model;
    auto *object = model.add_object();
    object->add_volume(cube(10));
    object->add_instance();
    SECTION("height range on the second nozzle") {
        DynamicPrintConfig upper_layers;
        upper_layers.set_key_value("extruder", new ConfigOptionInt(2));
        upper_layers.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        object->layer_config_ranges[{2.0, 10.0}].assign_config(std::move(upper_layers));
    }
    SECTION("custom tool change to the second nozzle") {
        CustomGCode::Item tool_change;
        tool_change.print_z  = 2.0;
        tool_change.type     = CustomGCode::ToolChange;
        tool_change.extruder = 2;
        model.plates_custom_gcodes[model.curr_plate_index].gcodes.push_back(tool_change);
    }

    DynamicPrintConfig config = firmware_retraction_config(false);
    Print print;
    arrange_objects_on_test_bed(model, config);
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    // Klipper's single "retracted" flag cannot hold the retraction of a parked nozzle.
    CHECK(print.validate().opt_key == "firmware_owns_retraction");

    config.set_key_value("firmware_owns_retraction", new ConfigOptionBool(true));
    print.apply(model, config);
    std::vector<StringObjectException> warnings;
    CHECK(print.validate(&warnings).string.empty());
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const StringObjectException &w) { return w.opt_key == "use_firmware_retraction"; }));
}

TEST_CASE("Slicer-owned firmware retraction rejects values Klipper refuses", "[GCodeWriter][Klipper][Retraction]")
{
    DynamicPrintConfig config = firmware_retraction_config(false);
    SECTION("negative retraction length") {
        config.set_key_value("retraction_length", new ConfigOptionFloats({0.5, -0.1}));
    }
    SECTION("negative extra length on restart") {
        config.set_key_value("retract_restart_extra", new ConfigOptionFloats({0., -0.1}));
    }
    SECTION("retraction speed below 1 mm/s") {
        config.set_key_value("retraction_speed", new ConfigOptionFloats({0.5, 25.}));
    }
    SECTION("deretraction speed below 1 mm/s") {
        config.set_key_value("deretraction_speed", new ConfigOptionFloats({0.5, 20.}));
    }
    Model model;
    auto *object = model.add_object();
    object->add_volume(cube(10));
    object->add_instance();
    Print print;
    arrange_objects_on_test_bed(model, config);
    object->ensure_on_bed();
    print.apply(model, config);
    CHECK_FALSE(print.validate().string.empty());

    config.set_key_value("firmware_owns_retraction", new ConfigOptionBool(true));
    print.apply(model, config);
    CHECK(print.validate().string.empty());
}

TEST_CASE("Klipper keeps square_corner_velocity and minimum_cruise_ratio by default", "[GCodeWriter][Klipper]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfKlipper));
    // Per-feature jerk values alone do not hand the corner velocity to the slicer.
    config.set_key_value("outer_wall_jerk", new ConfigOptionFloatsNullable({7.}));
    config.set_key_value("travel_jerk", new ConfigOptionFloatsNullable({12.}));
    const std::string gcode = Slic3r::Test::slice({TestMesh::cube_with_hole}, config);
    // The slicer still sets the per-feature acceleration.
    CHECK_THAT(gcode, Catch::Matchers::ContainsSubstring("SET_VELOCITY_LIMIT ACCEL="));
    CHECK(gcode.find("SQUARE_CORNER_VELOCITY") == std::string::npos);
    CHECK(gcode.find("MINIMUM_CRUISE_RATIO") == std::string::npos);
}

// Line numbers of the lines that are exactly `text`, or start with it when `prefix` is set.
static std::vector<size_t> line_numbers(const std::string &gcode, const std::string &text, bool prefix)
{
    std::vector<size_t> found;
    std::istringstream stream(gcode);
    std::string line;
    for (size_t number = 0; std::getline(stream, line); ++ number)
        if (prefix ? line.rfind(text, 0) == 0 : line == text)
            found.push_back(number);
    return found;
}

TEST_CASE("Klipper motion state is saved before the start G-code and restored before the end G-code", "[GCodeWriter][Klipper][RestoreState]")
{
    const std::string gcode = slice_cube({
        { "gcode_flavor",                "klipper" },
        { "klipper_restore_state",       "1" },
        { "machine_start_gcode",         ";TEST_START_GCODE" },
        { "machine_end_gcode",           ";TEST_END_GCODE" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "machine_max_acceleration_extruding", "20000,20000" },
        { "machine_max_acceleration_x",  "20000,20000" },
        { "machine_max_acceleration_y",  "20000,20000" },
        { "machine_max_jerk_x",          "20,20" },
        { "machine_max_jerk_y",          "20,20" },
        { "default_acceleration",        "6000" },
        { "outer_wall_acceleration",     "3000" },
        { "travel_acceleration",         "8000" },
        { "default_jerk",                "8" },
        { "accel_to_decel_enable",       "1" },
        { "accel_to_decel_factor",       "50%" },
        { "enable_pressure_advance",     "1" },
        { "pressure_advance",            "0.035" },
    });

    const std::vector<size_t> saves    = line_numbers(gcode, KLIPPER_SAVE_STATE_MACRO, false);
    const std::vector<size_t> restores = line_numbers(gcode, KLIPPER_RESTORE_STATE_MACRO, false);
    const std::vector<size_t> starts   = line_numbers(gcode, ";TEST_START_GCODE", false);
    const std::vector<size_t> ends     = line_numbers(gcode, ";TEST_END_GCODE", false);
    REQUIRE(saves.size() == 1);
    REQUIRE(restores.size() == 1);
    REQUIRE(starts.size() == 1);
    REQUIRE(ends.size() == 1);
    CHECK(saves.front() < starts.front());
    CHECK(starts.front() < restores.front());
    CHECK(restores.front() < ends.front());

    // Every state the configuration makes the slicer change lies between the save and the restore.
    std::vector<size_t> state_changes = line_numbers(gcode, "SET_VELOCITY_LIMIT", true);
    const std::vector<size_t> pressure_advance = line_numbers(gcode, "SET_PRESSURE_ADVANCE", true);
    REQUIRE_FALSE(state_changes.empty());
    REQUIRE_FALSE(pressure_advance.empty());
    CHECK_THAT(gcode, Catch::Matchers::ContainsSubstring("SQUARE_CORNER_VELOCITY=8"));
    CHECK_THAT(gcode, Catch::Matchers::ContainsSubstring("MINIMUM_CRUISE_RATIO=0.5"));
    state_changes.insert(state_changes.end(), pressure_advance.begin(), pressure_advance.end());
    for (const size_t line : state_changes) {
        CAPTURE(line);
        CHECK(line > saves.front());
        CHECK(line < restores.front());
    }
}

TEST_CASE("Klipper state macros are called only with klipper_restore_state", "[GCodeWriter][Klipper][RestoreState]")
{
    // The option is off by default: existing printer presets produce the G-code they did before.
    CHECK_FALSE(DynamicPrintConfig::full_print_config().opt_bool("klipper_restore_state"));
    const std::string gcode = slice_cube({
        { "gcode_flavor",                "klipper" },
        { "klipper_restore_state",       "0" },
        { "enable_pressure_advance",     "1" },
        { "pressure_advance",            "0.035" },
    });
    CHECK_THAT(gcode, Catch::Matchers::ContainsSubstring("SET_PRESSURE_ADVANCE"));
    CHECK(line_numbers(gcode, KLIPPER_SAVE_STATE_MACRO, true).empty());
    CHECK(line_numbers(gcode, KLIPPER_RESTORE_STATE_MACRO, true).empty());
}

TEST_CASE("The shipped Klipper macros define the state macros the G-code calls", "[GCodeWriter][Klipper][RestoreState]")
{
    namespace fs = boost::filesystem;
    const fs::path cfg = fs::path(std::string(TEST_DATA_DIR)) / ".." / ".." / "resources" / "klipper" / "ananas_slicer_state.cfg";
    REQUIRE(fs::is_regular_file(cfg));
    fs::ifstream stream(cfg);
    const std::string text{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    for (const char *macro : { KLIPPER_SAVE_STATE_MACRO, KLIPPER_RESTORE_STATE_MACRO }) {
        CAPTURE(macro);
        CHECK(line_numbers(text, std::string("[gcode_macro ") + macro + "]", false).size() == 1);
    }
}

TEST_CASE("Shipped process profiles leave the corner velocity and cruise ratio to Klipper", "[GCodeWriter][Klipper][Profiles]")
{
    namespace fs = boost::filesystem;
    std::vector<std::string> offenders;
    size_t checked = 0;
    for (fs::directory_iterator vendor(PROFILES_DIR); vendor != fs::directory_iterator(); ++vendor) {
        const fs::path process_dir = vendor->path() / "process";
        if (! fs::is_directory(process_dir))
            continue;
        for (fs::directory_iterator file(process_dir); file != fs::directory_iterator(); ++file) {
            if (file->path().extension() != ".json")
                continue;
            fs::ifstream stream(file->path());
            const nlohmann::json profile = nlohmann::json::parse(stream);
            ++ checked;
            const auto jerk = profile.find("default_jerk");
            if (jerk != profile.end() && jerk->is_string() && std::stod(jerk->get<std::string>()) != 0.)
                offenders.push_back(file->path().string() + ": default_jerk");
            const auto cruise = profile.find("accel_to_decel_enable");
            if (cruise != profile.end() && cruise->is_string() && cruise->get<std::string>() != "0")
                offenders.push_back(file->path().string() + ": accel_to_decel_enable");
        }
    }
    CAPTURE(offenders);
    REQUIRE(checked > 0);
    CHECK(offenders.empty());
}

TEST_CASE("Ironing moves with the top surface acceleration and jerk", "[GCodeWriter][Klipper][Ironing]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", "klipper"},
        {"ironing_type", "top"},
        {"machine_max_acceleration_extruding", "20000,20000"},
        {"machine_max_acceleration_x", "20000,20000"},
        {"machine_max_acceleration_y", "20000,20000"},
        {"machine_max_jerk_x", "20,20"},
        {"machine_max_jerk_y", "20,20"},
        {"default_acceleration", "3000"},
        {"top_surface_acceleration", "1234"},
        {"default_jerk", "9"},
        {"infill_jerk", "12"},
        {"top_surface_jerk", "6"},
    });
    const std::string gcode = Slic3r::Test::slice({TestMesh::cube_with_hole}, config);

    std::string type, accel, scv;
    int ironing_moves = 0;
    std::istringstream stream(gcode);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
        } else if (line.rfind("SET_VELOCITY_LIMIT", 0) == 0) {
            auto value = [&line](const std::string &name, std::string &out) {
                const auto pos = line.find(" " + name + "=");
                if (pos != std::string::npos)
                    out = line.substr(pos + name.size() + 2, line.find_first_of(" ;", pos + 1) - pos - name.size() - 2);
            };
            value("ACCEL", accel);
            value("SQUARE_CORNER_VELOCITY", scv);
        } else if (type == "Ironing" && line.rfind("G1 X", 0) == 0 && line.find(" E") != std::string::npos) {
            ++ ironing_moves;
            CHECK(accel == "1234");
            CHECK(scv == "6");
        }
    }
    REQUIRE(ironing_moves > 0);
}

TEST_CASE("Short hops between top surface lines keep the top surface acceleration and jerk", "[GCodeWriter][Klipper]")
{
    // Monotonic lines are joined by travels. Each of those used to switch to the travel
    // acceleration and back, two SET_VELOCITY_LIMIT commands for every line of a top surface.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", "klipper"},
        {"top_surface_pattern", "monotonicline"},
        {"machine_max_acceleration_extruding", "20000,20000"},
        {"machine_max_acceleration_x", "20000,20000"},
        {"machine_max_acceleration_y", "20000,20000"},
        {"machine_max_acceleration_travel", "20000,20000"},
        {"machine_max_jerk_x", "20,20"},
        {"machine_max_jerk_y", "20,20"},
        {"default_acceleration", "3000"},
        {"top_surface_acceleration", "1234"},
        {"travel_acceleration", "8000"},
        {"default_jerk", "9"},
        {"top_surface_jerk", "6"},
        {"travel_jerk", "15"},
        {"retraction_minimum_travel", "2"},
    });
    const std::string gcode = Slic3r::Test::slice({Slic3r::Test::cube(20)}, config);

    std::string type, accel, scv;
    double x = 0., y = 0.;
    int short_hops = 0, limit_commands = 0;
    std::istringstream stream(gcode);
    std::string line;
    auto number = [&line](char axis, double &out) {
        const auto pos = line.find(std::string(" ") + axis);
        if (pos != std::string::npos)
            out = std::atof(line.c_str() + pos + 2);
    };
    while (std::getline(stream, line)) {
        if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
        } else if (line.rfind("SET_VELOCITY_LIMIT", 0) == 0) {
            auto value = [&line](const std::string &name, std::string &out) {
                const auto pos = line.find(" " + name + "=");
                if (pos != std::string::npos)
                    out = line.substr(pos + name.size() + 2, line.find_first_of(" ;", pos + 1) - pos - name.size() - 2);
            };
            value("ACCEL", accel);
            value("SQUARE_CORNER_VELOCITY", scv);
            limit_commands += type == "Top surface";
        } else if (line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0) {
            const double from_x = x, from_y = y;
            number('X', x);
            number('Y', y);
            const bool travel = line.find(" E") == std::string::npos;
            const double length = std::hypot(x - from_x, y - from_y);
            if (type == "Top surface" && travel && length > 0. && length < 2.) {
                ++short_hops;
                CAPTURE(line, length);
                CHECK(accel == "1234");
                CHECK(scv == "6");
            }
        }
    }
    REQUIRE(short_hops > 20);
    // One command entering the top surface, not two per line.
    CHECK(limit_commands < 5);
}
