#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/KlipperMotionModel.hpp"

#include <cmath>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Expected values are derived from Klipper v0.13's planner equations (toolhead.py Move,
// LookAheadQueue) for the limits each test sets, not read back from the model.
namespace {

// Limits far from binding unless a test lowers one: the planner equations then reduce to the
// closed forms the tests state.
KlipperLimits open_limits(double max_accel, double minimum_cruise_ratio, double square_corner_velocity = 5.0)
{
    KlipperLimits limits;
    limits.max_velocity                  = 1000.0;
    limits.max_accel                     = max_accel;
    limits.minimum_cruise_ratio          = minimum_cruise_ratio;
    limits.square_corner_velocity        = square_corner_velocity;
    limits.max_z_velocity                = 1000.0;
    limits.max_z_accel                   = max_accel;
    limits.max_extrude_only_velocity     = 1000.0;
    limits.max_extrude_only_accel        = max_accel;
    limits.instantaneous_corner_velocity = 1.0;
    limits.pressure_advance              = 0.0;
    limits.pressure_advance_smooth_time  = 0.040;
    return limits;
}

std::vector<KlipperMotionModel::PlannedMove> drain(KlipperMotionModel& model)
{
    model.wait_moves();
    std::vector<KlipperMotionModel::PlannedMove> moves;
    for (size_t i = 0; i < model.planned_count(); ++i)
        moves.push_back(model.planned(i));
    model.pop_planned(model.planned_count());
    return moves;
}

double total_time(const std::vector<KlipperMotionModel::PlannedMove>& moves)
{
    double t = 0.0;
    for (const auto& m : moves)
        t += m.time();
    return t;
}

// Time of a move from rest to rest that cruises at v: d / v + v / a.
double rest_to_rest_time(double d, double v, double a) { return d / v + v / a; }

} // namespace

TEST_CASE("A single move from rest to rest takes distance over speed plus speed over acceleration",
          "[KlipperMotionModel]")
{
    // 100 mm at 100 mm/s, 1000 mm/s^2: 5 mm to accelerate, 5 mm to brake, the rest at speed.
    KlipperMotionModel model(open_limits(1000.0, 0.0));
    REQUIRE(model.add_move({ 100.0, 0.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 1);
    CHECK_THAT(moves[0].cruise_v, WithinRel(100.0, 1e-9));
    CHECK_THAT(moves[0].time(), WithinRel(rest_to_rest_time(100.0, 100.0, 1000.0), 1e-9));
}

TEST_CASE("A move too short to reach its speed peaks at sqrt(distance * accel) without a cruise ratio",
          "[KlipperMotionModel]")
{
    // 4 mm at 1000 mm/s^2 commanded at 100 mm/s: v_peak^2 = d * a = 4000, t = 2 * sqrt(d / a).
    KlipperMotionModel model(open_limits(1000.0, 0.0));
    REQUIRE(model.add_move({ 4.0, 0.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 1);
    CHECK_THAT(moves[0].cruise_v, WithinRel(std::sqrt(4000.0), 1e-9));
    CHECK_THAT(moves[0].time(), WithinRel(2.0 * std::sqrt(4.0 / 1000.0), 1e-9));
}

TEST_CASE("minimum_cruise_ratio keeps a short move cruising for that share of its length", "[KlipperMotionModel]")
{
    // Same 4 mm move, minimum_cruise_ratio 0.5: the smoothed pass caps v^2 at d * a * (1 - 0.5) = 2000,
    // so 1 mm accelerates, 2 mm cruise, 1 mm brakes.
    KlipperMotionModel model(open_limits(1000.0, 0.5));
    REQUIRE(model.add_move({ 4.0, 0.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 1);
    const double v = std::sqrt(2000.0);
    CHECK_THAT(moves[0].cruise_v, WithinRel(v, 1e-9));
    CHECK_THAT(moves[0].accel_distance(), WithinRel(1.0, 1e-9));
    CHECK_THAT(moves[0].cruise_t * moves[0].cruise_v, WithinRel(2.0, 1e-9));
    CHECK_THAT(moves[0].time(), WithinRel(2.0 * v / 1000.0 + 2.0 / v, 1e-9));
}

TEST_CASE("A right-angle corner is taken at the square corner velocity", "[KlipperMotionModel]")
{
    // Two 50 mm moves at 100 mm/s, 1000 mm/s^2, scv 5. jd = 25 * (sqrt(2) - 1) / 1000 and
    // v^2 = jd * a * sin(45) / (1 - sin(45)) = 25. The centripetal cap 2 * 50 * 1000 / 4 * tan(45)
    // = 25000 does not bind. Each move: 0.1 s up to speed, 0.095 s down to 5 mm/s, the rest cruising.
    KlipperMotionModel model(open_limits(1000.0, 0.0, 5.0));
    REQUIRE(model.add_move({ 50.0, 0.0, 0.0, 0.0 }, 100.0));
    REQUIRE(model.add_move({ 0.0, 50.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 2);
    CHECK_THAT(moves[0].end_v, WithinRel(5.0, 1e-9));
    CHECK_THAT(moves[1].start_v, WithinRel(5.0, 1e-9));
    const double brake_d  = (100.0 * 100.0 - 25.0) / 2000.0;
    const double one_move = 0.1 + 0.095 + (50.0 - 5.0 - brake_d) / 100.0;
    CHECK_THAT(total_time(moves), WithinRel(2.0 * one_move, 1e-9));
}

TEST_CASE("Corner speeds follow Klipper's junction deviation and centripetal limits", "[KlipperMotionModel]")
{
    // Move.calc_junction for a turn of `turn` degrees: sin(theta/2) = cos(turn/2) and
    // cos(theta/2) = sin(turn/2). A long straight lead-in and lead-out keep reachability from
    // binding, so the planned speed at the corner is sqrt(max_start_v2).
    const double turn   = GENERATE(1.0, 5.0, 30.0, 60.0, 90.0, 120.0, 150.0, 179.0);
    const double length = GENERATE(0.05, 1.0, 50.0);
    const double accel  = GENERATE(500.0, 5000.0, 20000.0);
    const double scv = 5.0, speed = 500.0;
    DYNAMIC_SECTION("turn " << turn << " length " << length << " accel " << accel)
    {
        KlipperMotionModel model(open_limits(accel, 0.0, scv));
        const double a_out = turn * M_PI / 180.0;
        REQUIRE(model.add_move({ 200.0, 0.0, 0.0, 0.0 }, speed));
        REQUIRE(model.add_move({ length, 0.0, 0.0, 0.0 }, speed));
        REQUIRE(model.add_move({ length * std::cos(a_out), length * std::sin(a_out), 0.0, 0.0 }, speed));
        REQUIRE(model.add_move({ 200.0 * std::cos(a_out), 200.0 * std::sin(a_out), 0.0, 0.0 }, speed));
        const auto moves = drain(model);
        REQUIRE(moves.size() == 4);

        const double sin_d2   = std::cos(turn * M_PI / 360.0);
        const double cos_d2   = std::sin(turn * M_PI / 360.0);
        const double jd       = scv * scv * (std::sqrt(2.0) - 1.0) / accel;
        const double jd_v2    = sin_d2 / (1.0 - sin_d2) * jd * accel;
        const double centr_v2 = 2.0 * length * accel * 0.25 * sin_d2 / cos_d2;
        const double expected = std::sqrt(std::min({ speed * speed, jd_v2, centr_v2 }));
        CHECK_THAT(moves[1].end_v, WithinRel(expected, 1e-4));
        CHECK_THAT(moves[2].start_v, WithinRel(expected, 1e-4));
    }
}

TEST_CASE("Going from extruding to travel is limited by the extruder's instantaneous corner velocity",
          "[KlipperMotionModel]")
{
    // Straight on, so only the extruder junction binds: E changes from 0.05 mm/mm to 0, and
    // extruder.py allows (instantaneous_corner_velocity / |0.05|)^2, i.e. 20 mm/s at 1 mm/s.
    KlipperMotionModel model(open_limits(1000.0, 0.0));
    REQUIRE(model.add_move({ 50.0, 0.0, 0.0, 2.5 }, 100.0));
    REQUIRE(model.add_move({ 50.0, 0.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 2);
    CHECK_THAT(moves[0].end_v, WithinRel(20.0, 1e-9));
}

TEST_CASE("Extrude-only moves are planned with the extrude-only limits and start and end at rest",
          "[KlipperMotionModel]")
{
    // A 0.8 mm retraction at 35 mm/s on a stock Voron extruder (max_extrude_only_accel 1863):
    // 0.8 / 35 + 35 / 1863. The travels around it neither carry speed into it nor out of it.
    KlipperLimits limits             = open_limits(7000.0, 0.5);
    limits.max_extrude_only_velocity = 79.8;
    limits.max_extrude_only_accel    = 1863.0;
    KlipperMotionModel model(limits);
    REQUIRE(model.add_move({ 50.0, 0.0, 0.0, 0.0 }, 200.0));
    REQUIRE(model.add_move({ 0.0, 0.0, 0.0, -0.8 }, 35.0));
    REQUIRE(model.add_move({ 50.0, 0.0, 0.0, 0.0 }, 200.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 3);
    CHECK_FALSE(moves[1].kinematic);
    CHECK_THAT(moves[0].end_v, WithinAbs(0.0, 1e-12));
    CHECK_THAT(moves[2].start_v, WithinAbs(0.0, 1e-12));
    CHECK_THAT(moves[1].time(), WithinRel(rest_to_rest_time(0.8, 35.0, 1863.0), 1e-9));
}

TEST_CASE("Moves with Z are slowed down to the Z limits scaled by the Z share of the move", "[KlipperMotionModel]")
{
    // Pure Z: z_ratio = 1, so 10 mm at max_z_velocity 15 and max_z_accel 350.
    KlipperLimits limits  = open_limits(7000.0, 0.5);
    limits.max_z_velocity = 15.0;
    limits.max_z_accel    = 350.0;
    KlipperMotionModel model(limits);
    REQUIRE(model.add_move({ 0.0, 0.0, 10.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 1);
    CHECK_THAT(moves[0].time(), WithinRel(rest_to_rest_time(10.0, 15.0, 350.0), 1e-9));
}

TEST_CASE("The commanded speed is clamped to max_velocity", "[KlipperMotionModel]")
{
    KlipperLimits limits = open_limits(1000.0, 0.0);
    limits.max_velocity  = 300.0;
    KlipperMotionModel model(limits);
    REQUIRE(model.add_move({ 200.0, 0.0, 0.0, 0.0 }, 350.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 1);
    CHECK_THAT(moves[0].cruise_v, WithinRel(300.0, 1e-9));
    CHECK_THAT(moves[0].time(), WithinRel(rest_to_rest_time(200.0, 300.0, 1000.0), 1e-9));
}

TEST_CASE("SET_VELOCITY_LIMIT changes the acceleration of the moves queued after it", "[KlipperMotionModel]")
{
    KlipperMotionModel model(open_limits(1000.0, 0.0));
    REQUIRE(model.add_move({ 100.0, 0.0, 0.0, 0.0 }, 100.0));
    model.wait_moves();
    model.set_velocity_limit(KlipperMotionModel::parse_set_velocity_limit("SET_VELOCITY_LIMIT ACCEL=4000"));
    REQUIRE(model.add_move({ 100.0, 0.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 2);
    CHECK_THAT(moves[0].time(), WithinRel(rest_to_rest_time(100.0, 100.0, 1000.0), 1e-9));
    CHECK_THAT(moves[1].time(), WithinRel(rest_to_rest_time(100.0, 100.0, 4000.0), 1e-9));
}

TEST_CASE("SET_VELOCITY_LIMIT follows Klipper's precedence for the cruise ratio", "[KlipperMotionModel]")
{
    KlipperMotionModel model(open_limits(4000.0, 0.5));

    SECTION("deprecated ACCEL_TO_DECEL converts against the ACCEL of the same command") {
        model.set_velocity_limit(KlipperMotionModel::parse_set_velocity_limit("SET_VELOCITY_LIMIT ACCEL=1000 ACCEL_TO_DECEL=250"));
        CHECK_THAT(model.max_accel(), WithinRel(1000.0, 1e-12));
        CHECK_THAT(model.minimum_cruise_ratio(), WithinRel(0.75, 1e-12));
    }
    SECTION("ACCEL_TO_DECEL alone converts against the current acceleration") {
        model.set_velocity_limit(KlipperMotionModel::parse_set_velocity_limit("SET_VELOCITY_LIMIT ACCEL_TO_DECEL=1000"));
        CHECK_THAT(model.minimum_cruise_ratio(), WithinRel(0.75, 1e-12));
    }
    SECTION("MINIMUM_CRUISE_RATIO wins over ACCEL_TO_DECEL") {
        model.set_velocity_limit(
            KlipperMotionModel::parse_set_velocity_limit("set_velocity_limit minimum_cruise_ratio=0.2 accel_to_decel=1000"));
        CHECK_THAT(model.minimum_cruise_ratio(), WithinRel(0.2, 1e-12));
    }
    SECTION("square corner velocity and velocity are taken over, out-of-range values are ignored") {
        model.set_velocity_limit(KlipperMotionModel::parse_set_velocity_limit(
            "SET_VELOCITY_LIMIT VELOCITY=250 SQUARE_CORNER_VELOCITY=8 MINIMUM_CRUISE_RATIO=1.5 ; comment ACCEL=1"));
        CHECK_THAT(model.max_velocity(), WithinRel(250.0, 1e-12));
        CHECK_THAT(model.square_corner_velocity(), WithinRel(8.0, 1e-12));
        CHECK_THAT(model.minimum_cruise_ratio(), WithinRel(0.5, 1e-12));
        CHECK_THAT(model.max_accel(), WithinRel(4000.0, 1e-12));
    }
}

TEST_CASE("M204 sets the acceleration only the ways Klipper accepts", "[KlipperMotionModel]")
{
    KlipperMotionModel model(open_limits(4000.0, 0.5));
    model.set_m204(std::nullopt, 1500.0, std::nullopt);
    CHECK_THAT(model.max_accel(), WithinRel(4000.0, 1e-12));
    model.set_m204(std::nullopt, 1500.0, 2500.0);
    CHECK_THAT(model.max_accel(), WithinRel(1500.0, 1e-12));
    model.set_m204(3000.0, 1000.0, 1000.0);
    CHECK_THAT(model.max_accel(), WithinRel(3000.0, 1e-12));
}

TEST_CASE("Switching pressure advance on or off drains the queue to a stop", "[KlipperMotionModel]")
{
    // extruder.py: a new smoothing window flushes step generation, so the toolhead stops between the
    // two collinear moves. A changed ADVANCE value with the same window does not.
    KlipperLimits limits    = open_limits(1000.0, 0.0);
    limits.pressure_advance = 0.04;
    KlipperMotionModel model(limits);

    REQUIRE(model.add_move({ 100.0, 0.0, 0.0, 0.0 }, 100.0));
    model.set_pressure_advance(0.05, std::nullopt);
    CHECK(model.queued_count() == 1);
    model.set_pressure_advance(0.0, std::nullopt);
    CHECK(model.queued_count() == 0);
    REQUIRE(model.add_move({ 100.0, 0.0, 0.0, 0.0 }, 100.0));
    const auto moves = drain(model);
    REQUIRE(moves.size() == 2);
    CHECK_THAT(moves[0].end_v, WithinAbs(0.0, 1e-12));
    CHECK_THAT(moves[1].start_v, WithinAbs(0.0, 1e-12));
}

TEST_CASE("A move without displacement is dropped", "[KlipperMotionModel]")
{
    KlipperMotionModel model(open_limits(1000.0, 0.5));
    CHECK_FALSE(model.add_move({ 0.0, 0.0, 0.0, 0.0 }, 100.0));
    CHECK(model.queued_count() == 0);
}

TEST_CASE("Lazy flushing commits a continuous plan for a long run of short moves", "[KlipperMotionModel]")
{
    // A long zig-zag queues many short moves, so the queue flushes lazily many times on the way. Every
    // move starts at the speed the previous one ended with, and covers exactly its own length.
    KlipperMotionModel model(open_limits(3000.0, 0.5, 5.0));
    size_t queued = 0;
    for (int i = 0; i < 2000; ++i) {
        const double a = (i % 7) * 0.3 + (i % 2 ? 0.5 : -0.5);
        REQUIRE(model.add_move({ 2.0 * std::cos(a), 2.0 * std::sin(a), 0.0, 0.08 }, 150.0));
        ++queued;
    }
    const size_t planned_before_drain = model.planned_count();
    CHECK(planned_before_drain > 0);
    const auto moves = drain(model);
    REQUIRE(moves.size() == queued);
    for (size_t i = 1; i < moves.size(); ++i)
        REQUIRE_THAT(moves[i].start_v, WithinRel(moves[i - 1].end_v, 1e-9));
    for (const auto& m : moves) {
        REQUIRE(std::isfinite(m.time()));
        REQUIRE(m.accel_t >= -1e-12);
        REQUIRE(m.cruise_t >= -1e-12);
        REQUIRE(m.decel_t >= -1e-12);
        REQUIRE_THAT(m.accel_distance() + m.cruise_t * m.cruise_v + m.decel_distance(), WithinRel(m.distance, 1e-9));
    }
    CHECK_THAT(moves.front().start_v, WithinAbs(0.0, 1e-12));
    CHECK_THAT(moves.back().end_v, WithinAbs(0.0, 1e-12));
}

TEST_CASE("Extended parameters are matched by name regardless of case and outside comments", "[KlipperMotionModel]")
{
    const std::optional<double> accel = KlipperMotionModel::extended_param("SET_VELOCITY_LIMIT accel=500", "ACCEL");
    REQUIRE(accel.has_value());
    CHECK_THAT(*accel, WithinAbs(500.0, 1e-12));
    CHECK_FALSE(KlipperMotionModel::extended_param("SET_VELOCITY_LIMIT ACCEL_TO_DECEL=500", "ACCEL").has_value());
    CHECK_FALSE(KlipperMotionModel::extended_param("SET_VELOCITY_LIMIT ; ACCEL=500", "ACCEL").has_value());
    CHECK_FALSE(KlipperMotionModel::extended_param("SET_VELOCITY_LIMIT ACCEL=fast", "ACCEL").has_value());
    const std::optional<double> smooth =
        KlipperMotionModel::extended_param("SET_PRESSURE_ADVANCE ADVANCE=0.04 SMOOTH_TIME=0.02", "SMOOTH_TIME");
    REQUIRE(smooth.has_value());
    CHECK_THAT(*smooth, WithinAbs(0.02, 1e-12));
}
