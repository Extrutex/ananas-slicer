#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/LifecycleEvents.hpp"

#include "libslic3r/Utils.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Timelapse g-code is emitted once per layer for every printer structure", "[Print][Regression]")
{
    struct PrinterCase {
        std::string name;
        std::string structure;
    };
    const PrinterCase printer = GENERATE(from_range(std::vector<PrinterCase>{
        { "undefined", "undefine" },
        { "CoreXY",    "corexy"   },
        { "i3",        "i3"       },
        { "H-Bot",     "hbot"     },
        { "Delta",     "delta"    },
    }));
    INFO("printer: " << printer.name);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "initial_layer_print_height", 0.2 },
        { "layer_change_gcode",          ";TEST_LAYER_CHANGE" },
        { "layer_height",                0.2 },
        { "printer_structure",           printer.structure },
        { "spiral_mode",                 false },
        { "time_lapse_gcode",            "TIMELAPSE_TAKE_FRAME" },
    });
    Print print;
    Model model;
    init_print({ cube(20) }, print, model, config);
    const std::string gcode = Slic3r::Test::gcode(print);

    const auto count = [&gcode](std::string_view token) {
        size_t occurrences = 0;
        size_t pos = 0;
        while ((pos = gcode.find(token, pos)) != std::string::npos) {
            ++occurrences;
            pos += token.size();
        }
        return occurrences;
    };

    const size_t layer_changes = count("\n;TEST_LAYER_CHANGE\n");
    REQUIRE(layer_changes > 0);
    CHECK(count("\nTIMELAPSE_TAKE_FRAME\n") == layer_changes);
}

SCENARIO("Changing the number of solid shell layers does not make all surfaces internal", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_shell_layers = 2 and bottom_shell_layers = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
            { "top_shell_layers",           2 },
            { "bottom_shell_layers",        1 },
            { "layer_height",               0.25 }, // get a known number of layers
            { "initial_layer_print_height", 0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({cube(20)}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (79, 78)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_shell_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

// Two copies of one object share a single slice. When a copy that has sliced itself becomes identical to an
// earlier object, it keeps its own layers while that object slices and only then takes the shared layers over.
TEST_CASE("An object that starts sharing another object's slices keeps its own layers until it takes them over", "[Print][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "raft_layers",                1 }, // each object gets support layers of its own
        { "sparse_infill_density",      "20%" }, // object_a's override below differs from this
    });
    // Activates the slicing-pipeline hook installed for the second slice below.
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"probe"}));

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    ModelObject *object_a = model.objects.front();
    // add_object() gives the copy extruder 1, and objects only share slices when their configs match.
    object_a->config.set_key_value("extruder", new ConfigOptionInt(1));
    ModelObject *object_b = model.add_object(*object_a); // shares object_a's mesh
    for (ModelInstance *instance : object_b->instances)
        instance->set_offset(instance->get_offset() + Vec3d(40., 0., 0.));
    // A per-object override makes the objects differ, so each slices itself.
    object_a->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));

    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 2);
    // The non-const accessors hand out the layer vectors themselves, which the checks below compare.
    PrintObject *print_b = print.get_object(1);
    REQUIRE(print_b->model_object()->id() == object_b->id()); // Print slices its own copy of the model
    REQUIRE(print_b->get_shared_object() == nullptr);
    const LayerPtrs        own_layers         = print_b->layers();
    const SupportLayerPtrs own_support_layers = print_b->support_layers();
    REQUIRE(!own_layers.empty());
    REQUIRE(!own_support_layers.empty());

    // Without the override the objects are identical again: print_a redoes the steps the setting invalidated,
    // and print_b shares it.
    object_a->config.erase("sparse_infill_density");
    print.apply(model, config);
    REQUIRE(print.objects().size() == 2);
    PrintObject *print_a = print.get_object(0);

    // Erasing the override invalidates print_a's posPrepareInfill and the steps after it. Its hook is the first one
    // print_a fires, and it runs after print_b starts sharing but before print_b takes the shared layers over.
    bool hook_fired          = false;
    bool shared_while_a_runs = false;
    bool kept_layers         = false;
    bool kept_support_layers = false;
    {
        Print::set_slicing_pipeline_hook_fn([&](Print &, const PrintObject *object, SlicingPipelineStepPlugin) {
            if (object != print_a || hook_fired)
                return;
            hook_fired          = true;
            shared_while_a_runs = print_b->get_shared_object() == print_a;
            kept_layers         = print_b->layers() == own_layers;
            kept_support_layers = print_b->support_layers() == own_support_layers;
        });
        const ScopeGuard reset_hook([] { Print::set_slicing_pipeline_hook_fn(nullptr); });
        print.process();
    }

    REQUIRE(hook_fired);
    CHECK(print.objects()[1] == print_b);
    CHECK(shared_while_a_runs);
    CHECK(kept_layers);
    CHECK(kept_support_layers);
    CHECK(print_b->get_shared_object() == print_a);
    REQUIRE(!print_a->layers().empty());
    CHECK(print_b->layers() == print_a->layers());
    CHECK(print_b->support_layers() == print_a->support_layers());
}

// ---------------------------------------------------------------------------
// Print::validate() warning collection
//
// validate() returns its warnings in a vector. The warning paths deliberately
// differ in how many entries they produce; these tests pin down each behaviour:
//   * independent checks    -> stack (one entry each)
//   * motion-ability        -> coalesce into one (mutually exclusive, gated)
//   * clumping detection    -> one independent warning
//   * layered clearance     -> many collisions concatenated into one entry
//   * null warnings pointer -> no-op, no crash, no blocking error
// ---------------------------------------------------------------------------
namespace {

// Build `n` 20mm cubes (spread apart, or stacked at the origin when `overlap`) into
// `model`/`print` and apply `config`, leaving the print ready to validate(). No slicing needed.
void build_cubes(Slic3r::Model& model, Slic3r::Print& print,
                 DynamicPrintConfig config, int n, bool overlap)
{
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n")); // validate() relative-E reset

    for (int i = 0; i < n; ++i) {
        ModelObject* object = model.add_object();
        object->add_volume(cube(20));
        ModelInstance* inst = object->add_instance();
        inst->set_offset(Vec3d(overlap ? 0.0 : i * 60.0, 0.0, 0.0));
    }
    for (ModelObject* mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
}

// Build cubes and run validate(), collecting warnings; returns the blocking error.
StringObjectException validate_cubes(const DynamicPrintConfig& config,
                                     std::vector<StringObjectException>& warnings,
                                     int n = 1, bool overlap = false)
{
    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, n, overlap);
    return print.validate(&warnings);
}

size_t count_opt_key(const std::vector<StringObjectException>& warnings, const std::string& key)
{
    return std::count_if(warnings.begin(), warnings.end(),
        [&](const StringObjectException& w) { return w.opt_key == key; });
}

// Make `default_acceleration` exceed the machine's extruding-acceleration limit.
void trigger_acceleration_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{ 100. });
    c.set_key_value("default_acceleration", new ConfigOptionFloatsNullable{ 100000. });
}

// Make `default_jerk` exceed the machine's jerk limit (junction deviation off so
// the jerk check is not skipped).
void trigger_jerk_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{ 0. });
    c.set_key_value("machine_max_jerk_x", new ConfigOptionFloats{ 1. });
    c.set_key_value("machine_max_jerk_y", new ConfigOptionFloats{ 1. });
    c.set_key_value("default_jerk", new ConfigOptionFloatsNullable{ 9999. });
}

// Precise outer wall is ignored unless the wall sequence is inner-outer.
void trigger_precise_wall_warning(DynamicPrintConfig& c)
{
    c.set_key_value("precise_outer_wall", new ConfigOptionBool(true));
    c.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::OuterInner));
}

} // namespace

// ---------------------------------------------------------------------------
// {first_object_name} filename placeholder
// ---------------------------------------------------------------------------
namespace {

// Add a printable 20mm cube named `name` to `model`; returns it so the caller can tweak it.
ModelObject* add_named_cube(Model& model, const std::string& name)
{
    ModelObject* obj = model.add_object();
    obj->name = name;
    obj->add_volume(make_cube(20.0, 20.0, 20.0));
    obj->add_instance();
    obj->ensure_on_bed();
    return obj;
}

// Resolve `format` to an output file name for a print of `model`. `filename_base`, when set,
// is the saved-project name passed to output_filename().
std::string resolved_output_name(Model& model, const std::string& format, const std::string& filename_base = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filename_format", new ConfigOptionString(format));

    Print print;
    for (ModelObject* obj : model.objects)
        print.auto_assign_extruders(obj);
    print.apply(model, config);
    return print.output_filename(filename_base);
}

struct ScopedLifecycleHook
{
    explicit ScopedLifecycleHook(LifecycleHookFn hook) { set_lifecycle_hook_fn(std::move(hook)); }
    ~ScopedLifecycleHook() { set_lifecycle_hook_fn(nullptr); }
};

} // namespace

TEST_CASE("Slicing lifecycle events identify the model", "[Print][LifecycleEvents]")
{
    struct ObservedEvent {
        LifecycleEvent event;
        std::string id;
        std::string name;
    };
    std::vector<ObservedEvent> events;
    ScopedLifecycleHook hook([&](LifecycleEvent event, const LifecycleEventContext& ctx) {
        events.push_back({ event, ctx.id, ctx.name });
    });

    Print print;
    Model model;
    ModelInfo info;
    info.model_name = "Lifecycle test model";
    model.model_info = std::make_shared<ModelInfo>(std::move(info));
    init_print({cube(20)}, print, model);

    print.process();
    ScopedTemporaryFile temp(".gcode");
    print.export_gcode(temp.string(), nullptr, nullptr);
    GCodeProcessorResult result;
    print.export_gcode_from_previous_file(temp.string(), &result);

    const std::string expected_id = std::to_string(print.model().id().id);
    const std::vector<LifecycleEvent> expected_events = {
        LifecycleEvent::SliceStarted,
        LifecycleEvent::SliceGeometryFinished,
        LifecycleEvent::GCodeExportStarted,
        LifecycleEvent::GCodeExportFinished,
        LifecycleEvent::GCodeExportStarted,
        LifecycleEvent::GCodeExportFinished,
    };
    REQUIRE(events.size() == expected_events.size());
    for (size_t i = 0; i < expected_events.size(); ++i) {
        CHECK(events[i].event == expected_events[i]);
        CHECK(events[i].id == expected_id);
        CHECK(events[i].name == "Lifecycle test model");
    }
}

TEST_CASE("Slicing lifecycle event name is empty without model metadata", "[Print][LifecycleEvents]")
{
    std::string event_id;
    std::string event_name = "unset";
    ScopedLifecycleHook hook([&](LifecycleEvent event, const LifecycleEventContext& ctx) {
        if (event == LifecycleEvent::SliceStarted) {
            event_id = ctx.id;
            event_name = ctx.name;
        }
    });

    Print print;
    Model model;
    init_print({cube(20)}, print, model);
    print.process();

    CHECK(event_id == std::to_string(print.model().id().id));
    CHECK(event_name.empty());
}

TEST_CASE("Output filenames with numeric statistics fail before slicing finishes", "[Print][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filename_format", new ConfigOptionString("{int(total_weight*10) / 10.0}"));

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);

    CHECK_THROWS_AS(print.output_filename(), PlaceholderParserError);
}

TEST_CASE("Print: {first_object_name} names the first printable object on the plate", "[Print]")
{
    Model model;

    SECTION("uses the object's name") {
        add_named_cube(model, "WidgetPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "WidgetPart.gcode");
    }

    SECTION("picks the first when several objects are printable") {
        add_named_cube(model, "FirstPart");
        add_named_cube(model, "SecondPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "FirstPart.gcode");
    }

    SECTION("skips objects outside the print volume (e.g. on another plate)") {
        // First in model order, but not on the current plate, so is_printable() is false.
        add_named_cube(model, "OtherPlatePart")->instances.front()->print_volume_state = ModelInstancePVS_Fully_Outside;
        add_named_cube(model, "OnPlatePart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "OnPlatePart.gcode");
    }

    SECTION("is empty when the object has no name") {
        add_named_cube(model, "");
        CHECK(resolved_output_name(model, "part_{first_object_name}") == "part_.gcode");
    }
}

TEST_CASE("Print: {first_object_name} is not replaced by the saved-project file name", "[Print]")
{
    // Passing a saved-project file name as the filename_base must not change {first_object_name}.
    Model model;
    add_named_cube(model, "WidgetPart");
    CHECK(resolved_output_name(model, "{first_object_name}", "SavedProject") == "WidgetPart.gcode");
}

TEST_CASE("Print::validate stacks independent warnings", "[Print][validate]")
{
    // Two unrelated checks (region precise-wall + machine acceleration) must each
    // contribute their own entry.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(warnings.size() >= 2);
    CHECK(count_opt_key(warnings, "precise_outer_wall") == 1);  // jump-to key is preserved
    for (const auto& w : warnings)
        CHECK(w.is_warning);                                   // every collected entry is a warning
}

TEST_CASE("Print::validate coalesces motion-ability warnings into one", "[Print][validate]")
{
    // The jerk/junction/acceleration checks are mutually exclusive (gated on a shared
    // key), so adding a second motion trigger must NOT add a second warning.
    DynamicPrintConfig accel_only = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_only);
    std::vector<StringObjectException> w_accel;
    CHECK(validate_cubes(accel_only, w_accel).string.empty());

    DynamicPrintConfig accel_and_jerk = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_and_jerk);
    trigger_jerk_warning(accel_and_jerk);
    std::vector<StringObjectException> w_both;
    CHECK(validate_cubes(accel_and_jerk, w_both).string.empty());

    CHECK(w_accel.size() >= 1);
    CHECK(w_both.size() == w_accel.size());  // the extra motion trigger collapses into the same warning
}

TEST_CASE("Print::validate reports the clumping-detection warning", "[Print][validate]")
{
    // A distinct single-shot path: clumping/wrapping detection without a prime tower warns
    // (and carries the enable_prime_tower jump-to key). enable_prime_tower must be off, as
    // the warning lives in the no-prime-tower branch.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(count_opt_key(warnings, "enable_prime_tower") == 1);
}

TEST_CASE("Print::validate concatenates layered-clearance collisions into one warning", "[Print][validate]")
{
    // In by-layer mode, layered_print_cleareance_valid folds every too-close pair into a
    // single warning entry (newline-joined), unlike the per-check stacking above. Isolate
    // that entry by type so unrelated default-config warnings don't affect the assertion.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings, /*n=*/3, /*overlap=*/true);

    CHECK(err.string.empty());
    auto is_layered = [](const StringObjectException& w) {
        return w.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT; };
    REQUIRE(std::count_if(warnings.begin(), warnings.end(), is_layered) == 1);  // 3 objects, 2 collisions, 1 entry
    auto it = std::find_if(warnings.begin(), warnings.end(), is_layered);
    CHECK(it->string.find('\n') != std::string::npos);  // the collisions were concatenated
}

TEST_CASE("Print::validate tolerates a null warnings pointer", "[Print][validate]")
{
    // Callers may pass no warnings sink: a warning-producing config must not crash
    // and must still return without a blocking error.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, /*n=*/1, /*overlap=*/false);

    StringObjectException err = print.validate();  // warnings == nullptr
    CHECK(err.string.empty());
}

TEST_CASE("A default slice emits perimeter, infill, and skirt", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 } // keep recorded Z at the printed height
    });
    CHECK(role_passes(gcode, "perimeter") > 0);
    CHECK(role_passes(gcode, "infill")    > 0);
    CHECK(role_passes(gcode, "skirt")     > 0);
    CHECK_THAT(max_z(gcode), Catch::Matchers::WithinAbs(20.0, 1e-4));
}

// The G-code carries a config-comment block describing the resolved settings. The
// per-region width lines are always present; the support and first-layer lines appear
// only when those features are configured.
TEST_CASE("G-code lists the resolved extrusion-width settings", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, { { "initial_layer_line_width", 0 } });
    CHECK(gcode.find("; external perimeters extrusion width") != std::string::npos);
    CHECK(gcode.find("; perimeters extrusion width")          != std::string::npos);
    CHECK(gcode.find("; infill extrusion width")              != std::string::npos);
    CHECK(gcode.find("; solid infill extrusion width")        != std::string::npos);
    CHECK(gcode.find("; top infill extrusion width")          != std::string::npos);
    CHECK(gcode.find("; support material extrusion width")    == std::string::npos);
    CHECK(gcode.find("; first layer extrusion width")         == std::string::npos);
    CHECK(gcode.find("; layer_height")                        != std::string::npos);
    CHECK(gcode.find("; sparse_infill_density")               != std::string::npos);

    const std::string with_support = slice({ cube(20) }, {
        { "initial_layer_line_width", 0 }, { "enable_support", true }, { "raft_layers", 3 },
    });
    CHECK(with_support.find("; support material extrusion width") != std::string::npos);

    const std::string with_first_layer = slice({ cube(20) }, { { "initial_layer_line_width", "0.5" } });
    CHECK(with_first_layer.find("; first layer extrusion width") != std::string::npos);
}

// gcode_skip_config_block suppresses the resolved-settings block while leaving the
// header and executable blocks intact.
TEST_CASE("gcode_skip_config_block omits the resolved-settings comment block", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "gcode_skip_config_block", true },
        { "gcode_comments",         true },
    });
    CHECK(gcode.find("; CONFIG_BLOCK_START")     == std::string::npos);
    CHECK(gcode.find("; CONFIG_BLOCK_END")       == std::string::npos);
    CHECK(gcode.find("; layer_height =")         == std::string::npos);
    CHECK(gcode.find("; fill_density =")         == std::string::npos);
    CHECK(gcode.find("; HEADER_BLOCK_START")     != std::string::npos);
    CHECK(gcode.find("; EXECUTABLE_BLOCK_START") != std::string::npos);
}

// Some firmwares only scan the last N lines of the file for "estimated printing time", so it
// must stay close to EOF regardless of the resolved-settings config block's size.
TEST_CASE("The estimated printing time comment stays near the end of the file", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {});
    const size_t config_block_end = gcode.find("; CONFIG_BLOCK_END");
    const size_t filament_stats   = gcode.find("; filament used [mm]");
    const size_t time_comment     = gcode.find("estimated printing time");
    REQUIRE(config_block_end != std::string::npos);
    REQUIRE(filament_stats != std::string::npos);
    REQUIRE(time_comment != std::string::npos);
    CHECK(filament_stats > config_block_end);
    CHECK(time_comment > filament_stats);

    const size_t line_start = gcode.rfind('\n', time_comment) + 1;
    const size_t trailing_lines = std::count(gcode.begin() + line_start, gcode.end(), '\n');
    CHECK(trailing_lines <= 5);
}

// Custom G-code templates substitute placeholders during export.
TEST_CASE("Custom G-code placeholders are substituted", "[Print]")
{
    // [current_extruder] in the start G-code.
    CHECK(slice({ cube(20) }, { { "machine_start_gcode", "; Extruder [current_extruder]" } })
              .find("; Extruder 0") != std::string::npos);

    // [layer_num] / [layer_z] in the end G-code (a 20mm cube at 0.1mm is 200 layers).
    const std::string end_gcode = slice({ cube(20) }, {
        { "machine_end_gcode",          "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(end_gcode.find("; Layer_num 199") != std::string::npos);
    CHECK(end_gcode.find("; Layer_z 20")    != std::string::npos);

    // printing_by_object_gcode is emitted between sequentially printed objects.
    CHECK(slice_two_cubes_arranged({
                    { "print_sequence",           "by object" },
                    { "printing_by_object_gcode", "; between-object-gcode" },
                })
              .find("; between-object-gcode") != std::string::npos);

    // [layer_num] keeps counting across sequentially printed objects (199 then 399).
    const std::string per_layer = slice_two_cubes_arranged({
        { "print_sequence",             "by object" },
        { "layer_change_gcode",         ";Layer:[layer_num] ([layer_z] mm)" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(per_layer.find(";Layer:199 ") != std::string::npos);
    CHECK(per_layer.find(";Layer:399 ") != std::string::npos);
}

TEST_CASE("export_gcode writes G-code without a result pointer", "[Print][export_gcode]")
{
    Print print;
    Model model;
    Slic3r::Test::init_print({cube(20)}, print, model);
    print.process();

    ScopedTemporaryFile temp(".gcode");
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), nullptr, nullptr));

    std::ifstream in(temp.string());
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE_FALSE(gcode.empty());
}

TEST_CASE("Exporting a sliced print again gives the same G-code", "[Print][export_gcode][Regression]")
{
    const int instances = GENERATE(1, 3);
    CAPTURE(instances);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    TestMesh           mesh   = TestMesh::ipadstand;
    SECTION("infill reversed by chaining") { config.set_deserialize_strict({{"sparse_infill_pattern", "gyroid"}}); }
    SECTION("support reversed by chaining") {
        mesh = TestMesh::overhang;
        config.set_deserialize_strict({{"enable_support", true}, {"support_interface_pattern", "concentric"}});
    }
    Print print;
    Model model;
    Slic3r::Test::init_print({Slic3r::Test::mesh(mesh)}, print, model, config, nullptr, true, instances);

    const auto export_without_timestamp = [&print]() {
        std::string gcode = Slic3r::Test::gcode(print);
        const size_t line = gcode.find("; generated by ");
        REQUIRE(line != std::string::npos);
        gcode.erase(line, gcode.find('\n', line) - line);
        return gcode;
    };
    const std::string first  = export_without_timestamp();
    const std::string second = export_without_timestamp();

    // Shows the first differing line on failure.
    const size_t diff       = std::mismatch(first.begin(), first.end(), second.begin(), second.end()).first - first.begin();
    const size_t line_start = diff == 0 ? 0 : first.rfind('\n', diff - 1) + 1;
    INFO("first export:  " << first.substr(line_start, first.find('\n', diff) - line_start));
    INFO("second export: " << second.substr(line_start, second.find('\n', diff) - line_start));
    CHECK(diff == first.size());
    CHECK(first.size() == second.size());
}

TEST_CASE("Sequential printing follows model order", "[Print]")
{
    // Two objects of different heights, taller one added first. Orca prints
    // sequential objects in model order, so the taller one is printed first.
    const std::string gcode = Slic3r::Test::slice({ cube(20), Slic3r::make_cube(20, 20, 10) }, {
        { "print_sequence",             "by object" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 }
    });

    // The first object's height is the peak Z reached before Z drops back to the
    // first layer (the object change). With by-object printing only an object
    // change returns Z to the bottom.
    double first_object_peak_z = 0.0;
    double running_peak        = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (first_object_peak_z != 0.0 || !line.extruding(self)) return; // ignore travels (e.g. start-gcode Z lift)
        if (running_peak > 1.0 && self.z() < 1.0)
            first_object_peak_z = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });

    REQUIRE_THAT(first_object_peak_z, Catch::Matchers::WithinAbs(20.0, 0.3));
}

// A sequential (by-object) print must publish the print-level nozzle group result just
// like a by-layer print, so custom g-code can index the per-nozzle placeholder tables
// (e.g. nozzle_diameter_at_nozzle_id[]) instead of failing on an empty vector.
TEST_CASE("Sequential printing publishes the nozzle group result", "[Print][MultiNozzle]")
{
    SECTION("process() publishes the result") {
        Print print;
        Model model;
        place_two_cubes_apart(60.0, { { "print_sequence", "by object" } }, print, model);
        print.process();
        REQUIRE(print.get_layered_nozzle_group_result() != nullptr);
    }

    SECTION("start g-code can index the per-nozzle diameter table") {
        const std::string gcode = slice_two_cubes_arranged({
            { "print_sequence",      "by object" },
            { "machine_start_gcode", "{if nozzle_diameter_at_nozzle_id[0] > 0}; SEQ-ND-OK\n{endif}" },
        });
        CHECK(gcode.find("; SEQ-ND-OK") != std::string::npos);
    }
}

TEST_CASE("Slicing errors are reported per object with the object's name", "[Print]")
{
    Print print;
    Model model;
    init_print({Slic3r::Test::cube(20.)}, print, model);
    // Lift the cube off the bed: its first layer is empty, which G-code export reports per object.
    ModelObject *object = model.objects.front();
    object->name = "floating cube";
    object->instances.front()->set_offset(object->instances.front()->get_offset() + Vec3d(0., 0., 2.));
    print.apply(model, DynamicPrintConfig::full_print_config());
    print.set_status_silent();

    ScopedTemporaryFile temp(".gcode");
    std::string message;
    try {
        print.process();
        print.export_gcode(temp.string(), nullptr, nullptr);
        FAIL("slicing did not report the empty first layer");
    } catch (const SlicingErrors &errors) {
        REQUIRE(errors.errors_.size() == 1);
        message = print.slicing_errors_message(errors);
    }
    CHECK(message.rfind("floating cube: ", 0) == 0);
    CHECK(message.find("empty first layer") != std::string::npos);
}

namespace {

std::vector<std::string> lines_starting_with(const std::string &gcode, const std::string &prefix)
{
    std::vector<std::string> found;
    std::istringstream in(gcode);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind(prefix, 0) == 0)
            found.push_back(line);
    return found;
}

size_t layer_change_count(const std::string &gcode)
{
    return lines_starting_with(gcode, ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change)).size();
}

// Seconds from the "1h 2m 3s" / "45s" / "0.500000s" form get_time_dhms() writes.
double dhms_seconds(const std::string &text)
{
    double seconds = 0.;
    std::istringstream in(text);
    double value;
    char   unit;
    while (in >> value >> unit) {
        switch (unit) {
        case 'd': seconds += value * 86400.; break;
        case 'h': seconds += value * 3600.; break;
        case 'm': seconds += value * 60.; break;
        case 's': seconds += value; break;
        default: FAIL("unexpected time unit '" << unit << "' in \"" << text << "\"");
        }
    }
    return seconds;
}

} // namespace

TEST_CASE("Klipper G-code reports the layer count and every layer change to print_stats", "[Print][Klipper]")
{
    const std::string gcode = Slic3r::Test::slice({cube(20)}, {
        { "gcode_flavor",               "klipper" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
    });
    const size_t layers = layer_change_count(gcode);
    REQUIRE(layers == 100);

    const auto totals = lines_starting_with(gcode, "SET_PRINT_STATS_INFO TOTAL_LAYER=");
    REQUIRE(totals.size() == 1);
    CHECK(totals.front() == "SET_PRINT_STATS_INFO TOTAL_LAYER=" + std::to_string(layers));

    const auto currents = lines_starting_with(gcode, "SET_PRINT_STATS_INFO CURRENT_LAYER=");
    REQUIRE(currents.size() == layers);
    for (size_t i = 0; i < currents.size(); ++i)
        CHECK(currents[i] == "SET_PRINT_STATS_INFO CURRENT_LAYER=" + std::to_string(i + 1));
    CHECK(gcode.find(totals.front()) < gcode.find(currents.front()));
}

TEST_CASE("Layer progress is left to the user's G-code when it already reports it", "[Print][Klipper]")
{
    const std::string gcode = Slic3r::Test::slice({cube(20)}, {
        { "gcode_flavor",               "klipper" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "machine_start_gcode",        "SET_PRINT_STATS_INFO TOTAL_LAYER=[total_layer_count]\nG28" },
        { "layer_change_gcode",         "SET_PRINT_STATS_INFO CURRENT_LAYER={layer_num + 1}" },
    });
    const size_t layers = layer_change_count(gcode);
    REQUIRE(layers == 100);
    const auto totals = lines_starting_with(gcode, "SET_PRINT_STATS_INFO TOTAL_LAYER=");
    REQUIRE(totals.size() == 1);
    CHECK(totals.front() == "SET_PRINT_STATS_INFO TOTAL_LAYER=" + std::to_string(layers));
    CHECK(lines_starting_with(gcode, "SET_PRINT_STATS_INFO CURRENT_LAYER=").size() == layers);
}

TEST_CASE("Only the part of the layer progress the user's G-code leaves out is reported", "[Print][Klipper]")
{
    // The user reports the current layer, from the filament start G-code rather than the layer change
    // one, and only mentions the total in a comment.
    const std::string gcode = Slic3r::Test::slice({cube(20)}, {
        { "gcode_flavor",               "klipper" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "machine_start_gcode",        "G28 ; SET_PRINT_STATS_INFO TOTAL_LAYER comes from the slicer" },
        { "filament_start_gcode",       "SET_PRINT_STATS_INFO CURRENT_LAYER=1" },
    });
    const size_t layers = layer_change_count(gcode);
    REQUIRE(layers == 100);
    const auto totals = lines_starting_with(gcode, "SET_PRINT_STATS_INFO TOTAL_LAYER=");
    REQUIRE(totals.size() == 1);
    CHECK(totals.front() == "SET_PRINT_STATS_INFO TOTAL_LAYER=" + std::to_string(layers));
    // One filament start, and no line per layer from the slicer.
    CHECK(lines_starting_with(gcode, "SET_PRINT_STATS_INFO CURRENT_LAYER=").size() == 1);
}

TEST_CASE("The header's first layer time is the time of the first layer's moves", "[Print][Regression]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    init_print({cube(20)}, print, model, {
        { "gcode_flavor",               "klipper" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
    });
    print.set_status_silent();
    const std::string gcode = Slic3r::Test::gcode(print);

    const std::string prefix = "; estimated first layer printing time (normal mode) = ";
    const auto header = lines_starting_with(gcode, prefix);
    REQUIRE(header.size() == 1);
    const double header_seconds = dhms_seconds(header.front().substr(prefix.size()));

    // Re-run the time estimate over the exported file, exactly as the preview would.
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream out(temp.string());
        out << gcode;
    }
    GCodeProcessor::s_IsBBLPrinter = false;
    GCodeProcessor processor;
    processor.apply_config(print.config());
    processor.process_file(temp.string());
    const double first_layer = processor.get_first_layer_time(PrintEstimatedStatistics::ETimeMode::Normal);
    const double prepare     = processor.get_prepare_time(PrintEstimatedStatistics::ETimeMode::Normal);

    // A 20 mm cube's first layer takes well over the second the start G-code alone (prepare time) does;
    // the header used to print the latter.
    REQUIRE(first_layer > 5.);
    CHECK(first_layer > prepare + 2.);
    // get_time_dhms truncates to whole seconds.
    CHECK_THAT(header_seconds, Catch::Matchers::WithinAbs(first_layer, 1.5));
}

TEST_CASE("The build height check counts the Z shrinkage compensation once", "[Print][validate][Regression]")
{
    // printable_height is 100 mm and Z shrinks to 95 %, so the object is sliced 100 / 95 times taller.
    // 92 mm become about 96.8 mm and fit; 97 mm become about 102.1 mm and only the compensation makes them too tall; 101 mm are too tall on their own.
    const auto [height, expected] = GENERATE(table<double, std::string>({
        { 92., "" },
        { 97., "because of material shrinkage compensation" },
        { 101., "exceeds the maximum build volume height." },
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "printable_height", 100. },
        { "filament_shrinkage_compensation_z", "95%" },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "layer_change_gcode", "G92 E0\n" }, // validate() relative-E reset
    });

    Print print;
    Model model;
    init_print({ make_cube(20., 20., height) }, print, model, config);
    const std::string error = print.validate().string;
    INFO(error);
    if (expected.empty())
        CHECK(error.empty());
    else
        CHECK(error.find(expected) != std::string::npos);
}

TEST_CASE("The build height check counts the raft", "[Print][validate][Regression]")
{
    // printable_height is 100 mm; three raft layers lift the object by 0.9 mm.
    const auto [height, fits] = GENERATE(table<double, bool>({ { 98.8, true }, { 99.8, false } }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "printable_height", 100. },
        { "raft_layers", 3 },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "layer_change_gcode", "G92 E0\n" }, // validate() relative-E reset
    });

    Print print;
    Model model;
    init_print({ make_cube(20., 20., height) }, print, model, config);
    const SlicingParameters &slicing_params = print.objects().front()->slicing_parameters();
    REQUIRE_THAT(slicing_params.object_print_z_min, Catch::Matchers::WithinAbs(0.9, EPSILON));
    const std::string error = print.validate().string;
    INFO(error);
    if (fits)
        CHECK(error.empty());
    else
        CHECK(error.find("maximum build volume height") != std::string::npos);
}
