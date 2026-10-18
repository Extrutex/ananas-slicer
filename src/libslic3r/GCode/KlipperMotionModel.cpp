#include "KlipperMotionModel.hpp"

#include "libslic3r/LocalesUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace Slic3r {

namespace {

// toolhead.py
constexpr double LOOKAHEAD_FLUSH_TIME = 0.250;
constexpr double BUFFER_TIME_HIGH     = 2.0;
// toolhead.py Move: velocity limit of an extrude-only move before the extruder limits it.
constexpr double EXTRUDE_ONLY_ACCEL   = 99999999.9;
// toolhead.py Move: below this XYZ length a move is extrude-only.
constexpr double MIN_KINEMATIC_LENGTH = .000000001;

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::optional<double> parse_finite(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    size_t pos = 0;
    const double value = string_to_double_decimal_point(text, &pos);
    if (pos != text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace

KlipperMotionModel::KlipperMotionModel(const KlipperLimits& limits)
    : m_max_velocity(limits.max_velocity)
    , m_max_accel(limits.max_accel)
    , m_min_cruise_ratio(limits.minimum_cruise_ratio)
    , m_square_corner_velocity(limits.square_corner_velocity)
    , m_max_z_velocity(limits.max_z_velocity)
    , m_max_z_accel(limits.max_z_accel)
    , m_max_e_velocity(limits.max_extrude_only_velocity)
    , m_max_e_accel(limits.max_extrude_only_accel)
    , m_instant_corner_v(limits.instantaneous_corner_velocity)
    , m_pressure_advance(limits.pressure_advance)
    , m_pressure_advance_smooth_time(limits.pressure_advance_smooth_time)
    , m_junction_flush(BUFFER_TIME_HIGH)
{
    calc_junction_deviation();
}

void KlipperMotionModel::calc_junction_deviation()
{
    const double scv2      = m_square_corner_velocity * m_square_corner_velocity;
    m_junction_deviation   = scv2 * (std::sqrt(2.) - 1.) / m_max_accel;
    m_max_accel_to_decel   = m_max_accel * (1. - m_min_cruise_ratio);
}

void KlipperMotionModel::set_velocity_limit(const VelocityLimit& limit)
{
    // Klipper rejects out-of-range values with a G-code error and changes nothing; the model ignores
    // them the same way.
    std::optional<double> min_cruise_ratio;
    if (limit.minimum_cruise_ratio && *limit.minimum_cruise_ratio >= 0. && *limit.minimum_cruise_ratio < 1.)
        min_cruise_ratio = limit.minimum_cruise_ratio;
    else if (!limit.minimum_cruise_ratio && limit.accel_to_decel && *limit.accel_to_decel > 0.) {
        // Deprecated ACCEL_TO_DECEL, converted against the ACCEL of the same command if there is one.
        const double accel = (limit.accel && *limit.accel > 0.) ? *limit.accel : m_max_accel;
        min_cruise_ratio   = 1. - std::min(1., *limit.accel_to_decel / accel);
    }
    if (limit.velocity && *limit.velocity > 0.)
        m_max_velocity = *limit.velocity;
    if (limit.accel && *limit.accel > 0.)
        m_max_accel = *limit.accel;
    if (limit.square_corner_velocity && *limit.square_corner_velocity >= 0.)
        m_square_corner_velocity = *limit.square_corner_velocity;
    if (min_cruise_ratio)
        m_min_cruise_ratio = *min_cruise_ratio;
    calc_junction_deviation();
}

void KlipperMotionModel::set_m204(std::optional<double> s, std::optional<double> p, std::optional<double> t)
{
    double accel;
    if (s && *s > 0.)
        accel = *s;
    else if (!s && p && t && *p > 0. && *t > 0.)
        accel = std::min(*p, *t);
    else
        return;
    m_max_accel = accel;
    calc_junction_deviation();
}

void KlipperMotionModel::set_pressure_advance(std::optional<double> advance, std::optional<double> smooth_time)
{
    const double new_advance = advance.value_or(m_pressure_advance);
    const double new_smooth  = smooth_time.value_or(m_pressure_advance_smooth_time);
    if (new_advance < 0. || new_smooth < 0. || new_smooth > .200)
        return; // Klipper's minval/maxval reject these without a state change
    const double old_window = (m_pressure_advance != 0.) ? m_pressure_advance_smooth_time : 0.;
    const double new_window = (new_advance != 0.) ? new_smooth : 0.;
    if (new_window != old_window)
        // note_step_generation_scan_time() -> flush_step_generation() -> _flush_lookahead()
        flush_lookahead();
    m_pressure_advance             = new_advance;
    m_pressure_advance_smooth_time = new_smooth;
}

void KlipperMotionModel::Move::limit_speed(double speed, double new_accel)
{
    const double speed2 = speed * speed;
    if (speed2 < max_cruise_v2) {
        max_cruise_v2 = speed2;
        min_move_t    = distance / speed;
    }
    accel           = std::min(accel, new_accel);
    delta_v2        = 2. * distance * accel;
    smooth_delta_v2 = std::min(smooth_delta_v2, delta_v2);
}

bool KlipperMotionModel::add_move(const std::array<double, 4>& axes_d_in, double speed)
{
    // toolhead.py Move.__init__
    Move move;
    std::array<double, 4> axes_d = axes_d_in;
    move.accel              = m_max_accel;
    move.junction_deviation = m_junction_deviation;
    double velocity         = std::min(speed, m_max_velocity);
    move.distance           = std::sqrt(axes_d[0] * axes_d[0] + axes_d[1] * axes_d[1] + axes_d[2] * axes_d[2]);
    double inv_distance;
    if (move.distance < MIN_KINEMATIC_LENGTH) {
        axes_d[0] = axes_d[1] = axes_d[2] = 0.;
        move.distance  = std::abs(axes_d[3]);
        inv_distance   = (move.distance != 0.) ? 1. / move.distance : 0.;
        move.accel     = EXTRUDE_ONLY_ACCEL;
        velocity       = speed;
        move.kinematic = false;
    } else
        inv_distance = 1. / move.distance;
    // toolhead.py ToolHead.move: a move without displacement is dropped.
    if (move.distance == 0. || velocity <= 0.)
        return false;
    for (size_t i = 0; i < 4; ++i)
        move.axes_r[i] = axes_d[i] * inv_distance;
    move.min_move_t      = move.distance / velocity;
    move.max_cruise_v2   = velocity * velocity;
    move.delta_v2        = 2. * move.distance * move.accel;
    move.smooth_delta_v2 = 2. * move.distance * m_max_accel_to_decel;

    // kinematics/corexy.py and cartesian.py check_move: Z is slower.
    if (move.kinematic && axes_d[2] != 0.) {
        const double z_ratio = move.distance / std::abs(axes_d[2]);
        move.limit_speed(m_max_z_velocity * z_ratio, m_max_z_accel * z_ratio);
    }
    // kinematics/extruder.py check_move: extrude-only moves and retractions are limited.
    if (axes_d[3] != 0.) {
        const double axis_r = move.axes_r[3];
        if ((axes_d[0] == 0. && axes_d[1] == 0.) || axis_r < 0.) {
            const double inv_extrude_r = 1. / std::abs(axis_r);
            move.limit_speed(m_max_e_velocity * inv_extrude_r, m_max_e_accel * inv_extrude_r);
        }
    }

    // LookAheadQueue.add_move
    m_queue.push_back(move);
    if (m_queue.size() == 1)
        return true;
    calc_junction(m_queue.back(), m_queue[m_queue.size() - 2]);
    m_junction_flush -= m_queue.back().min_move_t;
    if (m_junction_flush <= 0.)
        // Enough moves have been queued to reach the target flush time.
        flush(true);
    return true;
}

void KlipperMotionModel::calc_junction(Move& move, const Move& prev) const
{
    if (!move.kinematic || !prev.kinematic)
        return;
    // extruder.py PrinterExtruder.calc_junction
    const double diff_r      = move.axes_r[3] - prev.axes_r[3];
    const double extruder_v2 = (diff_r != 0.) ? std::pow(m_instant_corner_v / std::abs(diff_r), 2) : move.max_cruise_v2;
    double max_start_v2 = std::min({ extruder_v2, move.max_cruise_v2, prev.max_cruise_v2, prev.next_junction_v2,
                                     prev.max_start_v2 + prev.delta_v2 });
    // Find max velocity using "approximated centripetal velocity"
    const double junction_cos_theta = -(move.axes_r[0] * prev.axes_r[0] + move.axes_r[1] * prev.axes_r[1] +
                                        move.axes_r[2] * prev.axes_r[2]);
    const double sin_theta_d2           = std::sqrt(std::max(0.5 * (1.0 - junction_cos_theta), 0.));
    const double cos_theta_d2           = std::sqrt(std::max(0.5 * (1.0 + junction_cos_theta), 0.));
    const double one_minus_sin_theta_d2 = 1. - sin_theta_d2;
    if (one_minus_sin_theta_d2 > 0. && cos_theta_d2 > 0.) {
        const double R_jd        = sin_theta_d2 / one_minus_sin_theta_d2;
        const double move_jd_v2  = R_jd * move.junction_deviation * move.accel;
        const double pmove_jd_v2 = R_jd * prev.junction_deviation * prev.accel;
        // Approximated circle must contact moves no further than mid-move
        //   centripetal_v2 = .5 * move_d * accel * tan_theta_d2
        const double quarter_tan_theta_d2 = .25 * sin_theta_d2 / cos_theta_d2;
        const double move_centripetal_v2  = move.delta_v2 * quarter_tan_theta_d2;
        const double pmove_centripetal_v2 = prev.delta_v2 * quarter_tan_theta_d2;
        max_start_v2 = std::min({ max_start_v2, move_jd_v2, pmove_jd_v2, move_centripetal_v2, pmove_centripetal_v2 });
    }
    move.max_start_v2    = max_start_v2;
    move.max_smoothed_v2 = std::min(max_start_v2, prev.max_smoothed_v2 + prev.smooth_delta_v2);
}

void KlipperMotionModel::set_junction(Move& move, double start_v2, double cruise_v2, double end_v2)
{
    PlannedMove& plan         = move.plan;
    const double half_inv_accel = .5 / move.accel;
    const double accel_d        = (cruise_v2 - start_v2) * half_inv_accel;
    const double decel_d        = (cruise_v2 - end_v2) * half_inv_accel;
    const double cruise_d       = move.distance - accel_d - decel_d;
    plan.distance  = move.distance;
    plan.accel     = move.accel;
    plan.kinematic = move.kinematic;
    plan.start_v   = std::sqrt(start_v2);
    plan.cruise_v  = std::sqrt(cruise_v2);
    plan.end_v     = std::sqrt(end_v2);
    // Time is the distance divided by the average velocity.
    plan.accel_t  = accel_d / ((plan.start_v + plan.cruise_v) * 0.5);
    plan.cruise_t = cruise_d / plan.cruise_v;
    plan.decel_t  = decel_d / ((plan.end_v + plan.cruise_v) * 0.5);
}

void KlipperMotionModel::flush(bool lazy)
{
    // LookAheadQueue.flush
    m_junction_flush        = LOOKAHEAD_FLUSH_TIME;
    bool   update_flush_count = lazy;
    size_t flush_count        = m_queue.size();
    // Traverse queue from last to first move and determine maximum junction speed assuming the
    // robot comes to a complete stop after the last move.
    m_delayed.clear();
    double next_end_v2 = 0., next_smoothed_v2 = 0., peak_cruise_v2 = 0.;
    for (size_t i = m_queue.size(); i-- > 0;) {
        Move&        move                  = m_queue[i];
        const double reachable_start_v2    = next_end_v2 + move.delta_v2;
        const double start_v2              = std::min(move.max_start_v2, reachable_start_v2);
        const double reachable_smoothed_v2 = next_smoothed_v2 + move.smooth_delta_v2;
        const double smoothed_v2           = std::min(move.max_smoothed_v2, reachable_smoothed_v2);
        if (smoothed_v2 < reachable_smoothed_v2) {
            // It's possible for this move to accelerate
            if (smoothed_v2 + move.smooth_delta_v2 > next_smoothed_v2 || !m_delayed.empty()) {
                // This move can decelerate or this is a full accel move after a full decel move
                if (update_flush_count && peak_cruise_v2 != 0.) {
                    flush_count        = i;
                    update_flush_count = false;
                }
                peak_cruise_v2 = std::min(move.max_cruise_v2, (smoothed_v2 + reachable_smoothed_v2) * .5);
                if (!m_delayed.empty()) {
                    // Propagate peak_cruise_v2 to any delayed moves
                    if (!update_flush_count && i < flush_count) {
                        double mc_v2 = peak_cruise_v2;
                        for (auto it = m_delayed.rbegin(); it != m_delayed.rend(); ++it) {
                            mc_v2 = std::min(mc_v2, it->start_v2);
                            set_junction(m_queue[it->idx], std::min(it->start_v2, mc_v2), mc_v2, std::min(it->end_v2, mc_v2));
                        }
                    }
                    m_delayed.clear();
                }
            }
            if (!update_flush_count && i < flush_count) {
                const double cruise_v2 = std::min({ (start_v2 + reachable_start_v2) * .5, move.max_cruise_v2, peak_cruise_v2 });
                set_junction(move, std::min(start_v2, cruise_v2), cruise_v2, std::min(next_end_v2, cruise_v2));
            }
        } else {
            // Delay calculating this move until peak_cruise_v2 is known
            m_delayed.push_back({ i, start_v2, next_end_v2 });
        }
        next_end_v2      = start_v2;
        next_smoothed_v2 = smoothed_v2;
    }
    if (update_flush_count || flush_count == 0)
        return;
    if (!m_delayed.empty()) {
        // Moves at the head of the queue that never met a move able to accelerate. Klipper's own
        // queue cannot reach this state; should it ever, each move cruises at its own start speed
        // instead of being left unplanned.
        double mc_v2 = std::numeric_limits<double>::infinity();
        for (auto it = m_delayed.rbegin(); it != m_delayed.rend(); ++it) {
            if (it->idx >= flush_count)
                continue;
            mc_v2 = std::min({ mc_v2, it->start_v2, m_queue[it->idx].max_cruise_v2 });
            set_junction(m_queue[it->idx], std::min(it->start_v2, mc_v2), mc_v2, std::min(it->end_v2, mc_v2));
        }
        m_delayed.clear();
    }
    // toolhead.py _process_moves: the committed moves leave the queue in order.
    for (size_t i = 0; i < flush_count; ++i)
        m_planned.push_back(m_queue[i].plan);
    m_queue.erase(m_queue.begin(), m_queue.begin() + flush_count);
    m_special_queuing_state = false;
}

void KlipperMotionModel::flush_lookahead()
{
    // Transit from "NeedPrime"/"Priming"/"Drip"/main state to "NeedPrime"
    flush(false);
    m_special_queuing_state = true;
    m_junction_flush        = BUFFER_TIME_HIGH;
}

void KlipperMotionModel::wait_moves() { flush_lookahead(); }

void KlipperMotionModel::dwell()
{
    // toolhead.py get_last_move_time()
    if (m_special_queuing_state)
        flush_lookahead();
    else
        flush(false);
}

void KlipperMotionModel::pop_planned(size_t count)
{
    count = std::min(count, m_planned.size());
    m_planned.erase(m_planned.begin(), m_planned.begin() + count);
}

std::optional<double> KlipperMotionModel::extended_param(std::string_view line, std::string_view name)
{
    if (const size_t comment = line.find(';'); comment != std::string_view::npos)
        line = line.substr(0, comment);
    auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    // Skip the command word.
    size_t pos = 0;
    while (pos < line.size() && is_space(line[pos]))
        ++pos;
    while (pos < line.size() && !is_space(line[pos]))
        ++pos;
    std::optional<double> result;
    while (pos < line.size()) {
        while (pos < line.size() && is_space(line[pos]))
            ++pos;
        const size_t start = pos;
        while (pos < line.size() && !is_space(line[pos]))
            ++pos;
        const std::string_view token = line.substr(start, pos - start);
        const size_t eq = token.find('=');
        if (eq == std::string_view::npos || !iequals(token.substr(0, eq), name))
            continue;
        // A repeated parameter takes its last value, as in Klipper's parameter dict.
        result = parse_finite(token.substr(eq + 1));
    }
    return result;
}

KlipperMotionModel::VelocityLimit KlipperMotionModel::parse_set_velocity_limit(std::string_view line)
{
    VelocityLimit limit;
    limit.velocity               = extended_param(line, "VELOCITY");
    limit.accel                  = extended_param(line, "ACCEL");
    limit.square_corner_velocity = extended_param(line, "SQUARE_CORNER_VELOCITY");
    limit.minimum_cruise_ratio   = extended_param(line, "MINIMUM_CRUISE_RATIO");
    limit.accel_to_decel         = extended_param(line, "ACCEL_TO_DECEL");
    return limit;
}

} // namespace Slic3r
