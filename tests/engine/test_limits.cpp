#include <catch2/catch_all.hpp>

#include "engine/Limits.hpp"
#include "libslic3r/KlipperLimits.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::engine;
using Catch::Matchers::WithinAbs;

namespace {

// A config whose machine limits are the given ones; every other speed and acceleration is far below them,
// so a test sees only the key it raises.
DynamicPrintConfig config_with_machine_limits(double max_speed, double max_accel)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("machine_max_speed_x", new ConfigOptionFloats({ max_speed, max_speed }));
    config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({ max_accel, max_accel }));
    for (const char* key : { "default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration", "top_surface_acceleration",
                             "initial_layer_acceleration", "bridge_acceleration" })
        config.set_key_value(key, new ConfigOptionFloats({ 100. }));
    for (const char* key : { "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
                             "top_surface_speed", "bridge_speed", "initial_layer_speed", "gap_infill_speed",
                             "small_perimeter_speed", "support_speed", "travel_speed" })
        config.set_key_value(key, new ConfigOptionFloats({ 10. }));
    return config;
}

std::vector<LimitViolation> violations_of(const DynamicPrintConfig& config, const std::string& key, const std::string& kind)
{
    std::vector<LimitViolation> found;
    for (const LimitViolation& violation : check_limits(config))
        if (violation.key == key && violation.kind == kind)
            found.push_back(violation);
    return found;
}

} // namespace

TEST_CASE("an acceleration above the profile's max acceleration is exceeded", "[EngineLimits]")
{
    DynamicPrintConfig config = config_with_machine_limits(300., 5000.);
    config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats({ 8000. }));

    const auto found = violations_of(config, "outer_wall_acceleration", "machine");
    REQUIRE(found.size() == 1);
    CHECK(found.front().state == "exceeded");
    CHECK(found.front().limit_key == "printer.max_accel");
    CHECK(found.front().source == "profile");
    CHECK_THAT(found.front().value, WithinAbs(8000., 1e-9));
    CHECK_THAT(found.front().limit, WithinAbs(5000., 1e-9));
}

TEST_CASE("a value from 90 percent of the limit up to the limit is a caution", "[EngineLimits]")
{
    DynamicPrintConfig config = config_with_machine_limits(300., 5000.);

    config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats({ 4500. }));
    const auto at_ninety = violations_of(config, "outer_wall_acceleration", "machine");
    REQUIRE(at_ninety.size() == 1);
    CHECK(at_ninety.front().state == "caution");

    config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats({ 5000. }));
    const auto at_limit = violations_of(config, "outer_wall_acceleration", "machine");
    REQUIRE(at_limit.size() == 1);
    CHECK(at_limit.front().state == "caution");

    config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats({ 4400. }));
    CHECK(violations_of(config, "outer_wall_acceleration", "machine").empty());
}

TEST_CASE("a print speed above the profile's max velocity is exceeded", "[EngineLimits]")
{
    DynamicPrintConfig config = config_with_machine_limits(200., 5000.);
    config.set_key_value("travel_speed", new ConfigOptionFloats({ 250. }));

    const auto found = violations_of(config, "travel_speed", "machine");
    REQUIRE(found.size() == 1);
    CHECK(found.front().state == "exceeded");
    CHECK(found.front().limit_key == "printer.max_velocity");
    CHECK(found.front().unit == "mm/s");
}

TEST_CASE("an imported Klipper snapshot replaces the profile's machine limits and names its source", "[EngineLimits]")
{
    DynamicPrintConfig config = config_with_machine_limits(300., 9000.);
    KlipperLimits      limits = klipper_limits_from_config(config);
    limits.max_accel          = 3000.;
    store_klipper_limits(limits, KlipperLimitsSource::Moonraker, "test", config);
    config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats({ 5000. }));

    const auto found = violations_of(config, "outer_wall_acceleration", "machine");
    REQUIRE(found.size() == 1);
    CHECK(found.front().source == "moonraker");
    CHECK_THAT(found.front().limit, WithinAbs(3000., 1e-9));
}

TEST_CASE("a profile without machine limits and without a snapshot has nothing to compare with", "[EngineLimits]")
{
    DynamicPrintConfig config = config_with_machine_limits(0., 0.);
    config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats({ 99999. }));

    CHECK(check_limits(config).empty());
}
