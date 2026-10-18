#pragma once

#include "libslic3r/KlipperLimits.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

namespace Slic3r {

// Model of Klipper v0.13 motion planning, not firmware.
//
// A port of the parts of Klipper that decide how long a move takes: toolhead.py (Move,
// LookAheadQueue with its smoothed minimum_cruise_ratio pass and lazy flushing, the
// SET_VELOCITY_LIMIT / M204 state), the extruder junction and extrude-only limits of
// kinematics/extruder.py, and the Z limit of the cartesian and corexy kinematics. Input shaping
// and pressure advance do not change move durations in Klipper and are not modelled; the only
// timing effect of SET_PRESSURE_ADVANCE, the full flush when the step generation scan window
// changes, is.
//
// The caller feeds moves in G-code order and collects the planned moves, in the same order, as
// Klipper commits them. Every queued move is eventually planned: lazily while moves keep coming,
// completely on wait_moves() and dwell().
class KlipperMotionModel
{
public:
    struct PlannedMove
    {
        double distance{ 0.0 };  // mm, XYZ length, or |E| for an extrude-only move
        double accel{ 0.0 };     // mm/s^2
        double start_v{ 0.0 };   // mm/s
        double cruise_v{ 0.0 };  // mm/s
        double end_v{ 0.0 };     // mm/s
        double accel_t{ 0.0 };   // s
        double cruise_t{ 0.0 };  // s
        double decel_t{ 0.0 };   // s
        bool   kinematic{ false };

        double time() const { return accel_t + cruise_t + decel_t; }
        double accel_distance() const { return 0.5 * (start_v + cruise_v) * accel_t; }
        double decel_distance() const { return 0.5 * (end_v + cruise_v) * decel_t; }
    };

    // SET_VELOCITY_LIMIT parameters; an absent parameter leaves the value unchanged.
    struct VelocityLimit
    {
        std::optional<double> velocity;
        std::optional<double> accel;
        std::optional<double> square_corner_velocity;
        std::optional<double> minimum_cruise_ratio;
        std::optional<double> accel_to_decel;
    };

    // Klipper's gcode_move speed before the first F word, mm/s.
    static constexpr double default_speed = 25.0;

    explicit KlipperMotionModel(const KlipperLimits& limits);

    // toolhead.py cmd_SET_VELOCITY_LIMIT.
    void set_velocity_limit(const VelocityLimit& limit);
    // toolhead.py cmd_M204: S sets the acceleration, otherwise the smaller of P and T, and only
    // when both are given. Klipper rejects anything else without changing state.
    void set_m204(std::optional<double> s, std::optional<double> p, std::optional<double> t);
    // extruder.py _set_pressure_advance: a change of the effective smoothing window flushes the
    // lookahead queue completely, which stops the toolhead.
    void set_pressure_advance(std::optional<double> advance, std::optional<double> smooth_time);

    // Queues a move given by its XYZE displacement in mm and requested speed in mm/s. Returns false
    // for a move without displacement, which Klipper drops; it is never planned.
    bool add_move(const std::array<double, 4>& axes_d, double speed);

    // toolhead.py wait_moves(): M400, and everything that waits for the queue to drain.
    void wait_moves();
    // toolhead.py dwell() up to the delay itself, which the caller accounts for: G4.
    void dwell();

    size_t             planned_count() const { return m_planned.size(); }
    const PlannedMove& planned(size_t idx) const { return m_planned[idx]; }
    // Drops the oldest `count` planned moves once the caller has consumed them.
    void               pop_planned(size_t count);
    // Moves queued but not planned yet.
    size_t             queued_count() const { return m_queue.size(); }

    double max_velocity() const { return m_max_velocity; }
    double max_accel() const { return m_max_accel; }
    double minimum_cruise_ratio() const { return m_min_cruise_ratio; }
    double square_corner_velocity() const { return m_square_corner_velocity; }

    // Reads the parameters of a SET_VELOCITY_LIMIT line.
    static VelocityLimit parse_set_velocity_limit(std::string_view line);
    // Value of a NAME=VALUE parameter of a Klipper extended command line, matched by name without
    // regard to case as Klipper does, with any ';' comment ignored. std::nullopt when the parameter
    // is absent or its value is not a number.
    static std::optional<double> extended_param(std::string_view line, std::string_view name);

private:
    struct Move
    {
        std::array<double, 4> axes_r{ 0.0, 0.0, 0.0, 0.0 };
        double distance{ 0.0 };
        double accel{ 0.0 };
        double junction_deviation{ 0.0 };
        double min_move_t{ 0.0 };
        double max_start_v2{ 0.0 };
        double max_cruise_v2{ 0.0 };
        double delta_v2{ 0.0 };
        double max_smoothed_v2{ 0.0 };
        double smooth_delta_v2{ 0.0 };
        double next_junction_v2{ 999999999.9 };
        bool   kinematic{ true };
        PlannedMove plan;

        void limit_speed(double speed, double accel);
    };
    struct Delayed
    {
        size_t idx;
        double start_v2;
        double end_v2;
    };

    void calc_junction_deviation();
    void calc_junction(Move& move, const Move& prev) const;
    static void set_junction(Move& move, double start_v2, double cruise_v2, double end_v2);
    void flush(bool lazy);
    // toolhead.py _flush_lookahead().
    void flush_lookahead();

    // Toolhead state that SET_VELOCITY_LIMIT and M204 change.
    double m_max_velocity;
    double m_max_accel;
    double m_min_cruise_ratio;
    double m_square_corner_velocity;
    double m_junction_deviation{ 0.0 };
    double m_max_accel_to_decel{ 0.0 };
    // Fixed at config time in Klipper.
    double m_max_z_velocity;
    double m_max_z_accel;
    double m_max_e_velocity;
    double m_max_e_accel;
    double m_instant_corner_v;
    // Pressure advance state, only for the scan window it implies.
    double m_pressure_advance;
    double m_pressure_advance_smooth_time;

    // LookAheadQueue.
    std::deque<Move>     m_queue;
    std::vector<Delayed> m_delayed;
    double               m_junction_flush;
    // The toolhead is in its "NeedPrime"/"Priming" state: nothing has been committed since the
    // last full flush.
    bool                 m_special_queuing_state{ true };

    std::deque<PlannedMove> m_planned;
};

} // namespace Slic3r
