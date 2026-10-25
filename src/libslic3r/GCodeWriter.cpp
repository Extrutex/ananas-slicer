#include "GCodeWriter.hpp"
#include "CustomGCode.hpp"
#include "I18N.hpp"
#include "PrintConfig.hpp"
#include "ClipperUtils.hpp"
#include "Geometry/ArcWelder.hpp"
#include "Line.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <assert.h>
#include <GCode/GCodeProcessor.hpp>

#ifdef __APPLE__
    #include <boost/spirit/include/karma.hpp>
#endif

namespace Slic3r {

bool GCodeWriter::full_gcode_comment = true;

void GCodeWriter::apply_print_config(const PrintConfig &print_config)
{
    this->config.apply(print_config, true);

    // Some machine limits are stride-2 (normal, silent) pairs, here we extract the value that will be used,
    // which is always normal mode at the moment
    // TODO: support silent? Any printer actually have that?
    auto get_machine_limits = [](const std::string key, const ConfigOptionFloats& opt) -> std::vector<double> {
        unsigned int stride = 1;
        unsigned int offset = 0;
        if (printer_options_with_variant_2.count(key) > 0) {
            stride = 2;
            // offset = <TODO: current print mode>;
        }

        std::vector<double> results;
        results.reserve(opt.values.size() / stride);

        for (unsigned int i = offset; i < opt.values.size(); i += stride) {
            results.emplace_back(opt.values[i]);
        }

        return results;
    };
    auto rounded = [](std::vector<double>&& vec) -> std::vector<double>&&{
        std::transform(vec.cbegin(), vec.cend(), vec.begin(), [](const double v) { return std::round(v); });
        return std::move(vec);
    };
    auto to_uint = [](const std::vector<double>& vec) {
        std::vector<unsigned int> r;
        std::transform(vec.begin(), vec.end(), std::back_inserter(r), [](const double v) { return static_cast<unsigned int>(v); });
        return r;
    };
#define LIMITS(OPT) get_machine_limits(#OPT, print_config.OPT)
#define LIMITS_UINT(OPT) to_uint(rounded(LIMITS(OPT)))

    m_single_extruder_multi_material = print_config.single_extruder_multi_material.value;
    // SET_VELOCITY_LIMIT ACCEL= applies to all moves, so the effective cap is the minimum of the
    // extruding limit and the per-axis X/Y limits. This honours user-configured Motion Ability
    // limits (#12244).
    auto extruding_limit = LIMITS_UINT(machine_max_acceleration_extruding);
    auto x_limit = LIMITS_UINT(machine_max_acceleration_x);
    auto y_limit = LIMITS_UINT(machine_max_acceleration_y);
    for (size_t i = 0; i < extruding_limit.size(); i++) {
        if (x_limit[i] > 0) extruding_limit[i] = std::min(extruding_limit[i], x_limit[i]);
        if (y_limit[i] > 0) extruding_limit[i] = std::min(extruding_limit[i], y_limit[i]);
    }
    m_max_acceleration = std::move(extruding_limit);
    m_max_jerk_x       = rounded(LIMITS(machine_max_jerk_x));
    m_max_jerk_y       = rounded(LIMITS(machine_max_jerk_y));
    m_resolution = print_config.resolution.value;
#undef LIMITS
#undef LIMITS_UINT
    // Orca: capture the printable area(s) so a spiral lift can be skipped when its
    // circle would leave the boundary and collide with the print limits. Full polygons
    // are stored (not a bounding box) so the check stays correct for non-rectangular
    // beds, and per-extruder areas are kept so printers with different boundaries per
    // extruder use the right limit for whichever extruder is active.
    auto to_scaled_polygon = [](const Pointfs &pts) {
        Polygon poly;
        poly.points.reserve(pts.size());
        for (const Vec2d &p : pts)
            poly.points.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())));
        poly.make_counter_clockwise();
        return poly;
    };

    m_bed_printable_area.points.clear();
    m_extruder_printable_areas.clear();

    if (print_config.printable_area.values.size() >= 3)
        m_bed_printable_area = to_scaled_polygon(print_config.printable_area.values);

    const std::vector<Pointfs> &extruder_areas = print_config.extruder_printable_area.values;
    if (!extruder_areas.empty()) {
        m_extruder_printable_areas.resize(extruder_areas.size());
        for (size_t i = 0; i < extruder_areas.size(); ++i) {
            if (extruder_areas[i].size() < 3) {
                // No dedicated area for this extruder: it can reach the whole bed.
                m_extruder_printable_areas[i] = m_bed_printable_area;
                continue;
            }
            Polygon extruder_poly = to_scaled_polygon(extruder_areas[i]);
            if (m_bed_printable_area.points.size() < 3) {
                m_extruder_printable_areas[i] = std::move(extruder_poly);
                continue;
            }
            // The reachable area is the extruder area clipped to the bed. Bed shapes are
            // convex in practice, so keep the largest resulting contour.
            Polygons clipped = intersection(extruder_poly, m_bed_printable_area);
            const Polygon *largest = nullptr;
            double          best_area = 0.;
            for (const Polygon &p : clipped) {
                double a = std::abs(p.area());
                if (a > best_area) { best_area = a; largest = &p; }
            }
            m_extruder_printable_areas[i] = largest ? *largest : std::move(extruder_poly);
        }
    }
}

const Polygon *GCodeWriter::active_printable_area() const
{
    if (const Extruder *e = this->filament()) {
        size_t id = e->extruder_id();
        if (id < m_extruder_printable_areas.size() && m_extruder_printable_areas[id].points.size() >= 3)
            return &m_extruder_printable_areas[id];
    }
    if (m_bed_printable_area.points.size() >= 3)
        return &m_bed_printable_area;
    return nullptr;
}

bool GCodeWriter::spiral_lift_fits_printable_area(const Vec2d &center, double radius) const
{
    const Polygon *area = this->active_printable_area();
    if (area == nullptr)
        return true; // Boundary unknown: don't restrict (preserve previous behavior).

    const Point  c        = Point::new_scale(center.x(), center.y());
    const double r_scaled = scale_(radius);
    const double r2       = r_scaled * r_scaled;

    // The spiral traces a full circle of `radius` around `center`, so the center must lie
    // inside the printable area and every edge must be at least `radius` away from it.
    if (!area->contains(c))
        return false;
    const Points &pts = area->points;
    for (size_t i = 0, n = pts.size(); i < n; ++i)
        if (Line::distance_to_squared(c, pts[i], pts[(i + 1) % n]) < r2)
            return false;
    return true;
}

void GCodeWriter::set_extruders(std::vector<unsigned int> extruder_ids)
{
    std::sort(extruder_ids.begin(), extruder_ids.end());
    m_filament_extruders.clear();
    //ORCA: Reset current extruder ID and clear pointers to prevent dangling pointers when extruders are recreated.
    m_curr_extruder_id = -1;
    m_cached_extruder_idx = 0;
    std::fill(m_curr_filament_extruder.begin(), m_curr_filament_extruder.end(), nullptr);
    m_filament_extruders.reserve(extruder_ids.size());
    for (unsigned int extruder_id : extruder_ids)
        m_filament_extruders.emplace_back(Extruder(extruder_id, &this->config, config.single_extruder_multi_material.value));

    /*  we enable support for multiple extruder if any extruder greater than 0 is used
        (even if prints only uses that one) since we need to output Tx commands
        first extruder has index 0 */
    //ORCA: Fix undefined behavior by checking if the vector is empty before taking max_element.
    this->multiple_extruders = !extruder_ids.empty() && (*std::max_element(extruder_ids.begin(), extruder_ids.end())) > 0;
}

std::string GCodeWriter::preamble()
{
    std::ostringstream gcode;
    gcode << "G90\n";
    gcode << "G21\n";
    if (this->config.use_relative_e_distances) {
        gcode << "M83 ; use relative distances for extrusion\n";
    } else {
        gcode << "M82 ; use absolute distances for extrusion\n";
    }
    gcode << this->reset_e(true);
    return gcode.str();
}

std::string GCodeWriter::format_temperature(unsigned int temperature, bool wait, int tool, std::string comment)
{
    if (comment.empty())
        comment = wait ? "set nozzle temperature and wait for it to be reached" : "set nozzle temperature";
    std::ostringstream gcode;
    gcode << (wait ? "M109" : "M104") << " S" << temperature;
    if (tool != -1)
        gcode << " T" << tool;
    gcode << " ; " << comment << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_temperature(unsigned int temperature, bool wait, int tool) const
{
    // set tool to -1 to make sure we won't emit T parameter for single extruder or SEMM
    if (!this->multiple_extruders || m_single_extruder_multi_material)
        tool = -1;
    return format_temperature(temperature, wait, tool);
}

// BBS
std::string GCodeWriter::set_bed_temperature(int temperature, bool wait)
{
    if (temperature == m_last_bed_temperature && (! wait || m_last_bed_temperature_reached))
        return std::string();

    m_last_bed_temperature = temperature;
    m_last_bed_temperature_reached = wait;

    std::string code, comment;
    std::ostringstream gcode;

    if (wait) {
        code = "M190";
        comment = "set bed temperature and wait for it to be reached";
    }
    else {
        code = "M140";
        comment = "set bed temperature";
    }

    gcode << code << " S" << temperature << " ; " << comment << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_chamber_temperature(int temperature, bool wait)
{
    std::string code, comment;
    std::ostringstream gcode;

    if (wait)
    {
        // Orca: should we let the M191 command to turn on the auxiliary fan?
        if (config.auxiliary_fan)
            gcode << "M106 P2 S255 \n";
        gcode << "M191 S" << std::to_string(temperature) << " ;"
              << "set chamber_temperature and wait for it to be reached\n";
        if (config.auxiliary_fan)
            gcode << "M106 P2 S0 \n";
    }
    else {
        code = "M141";
        comment = "set chamber_temperature";
        gcode << code << " S" << temperature << ";" << comment << "\n";
    }
    return gcode.str();
}

#define EXTRUDER_LIMIT(OPT) \
    (filament() ? ((OPT).size() <= filament()->extruder_id() ? 0 : (OPT)[filament()->extruder_id()]) : \
                  ((OPT).empty() ? 0 : *std::max_element((OPT).cbegin(), (OPT).cend())))

std::string GCodeWriter::set_print_acceleration(unsigned int acceleration)
{
    // Clamp the acceleration to the allowed maximum.
    if (EXTRUDER_LIMIT(m_max_acceleration) > 0 && acceleration > EXTRUDER_LIMIT(m_max_acceleration))
        acceleration = EXTRUDER_LIMIT(m_max_acceleration);
    if (acceleration == 0 || acceleration == m_last_acceleration)
        return std::string();
    m_last_acceleration = acceleration;

    std::ostringstream gcode;
    gcode << "SET_VELOCITY_LIMIT ACCEL=" << acceleration;
    if (this->config.accel_to_decel_enable) {
        gcode << " MINIMUM_CRUISE_RATIO=" << (1.0 - this->config.accel_to_decel_factor / 100.0);
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; adjust minimum cruise ratio";
    }
    if (GCodeWriter::full_gcode_comment) gcode << " ; adjust acceleration";
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_jerk_xy(double jerk)
{
    if (jerk < 0.01 || is_approx(jerk, m_last_jerk))
        return std::string();
    m_last_jerk = jerk;

    // Klipper has one corner velocity for both axes. Clamp it to the allowed maximum.
    if (EXTRUDER_LIMIT(m_max_jerk_x) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_x))
        jerk = EXTRUDER_LIMIT(m_max_jerk_x);
    if (EXTRUDER_LIMIT(m_max_jerk_y) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_y))
        jerk = EXTRUDER_LIMIT(m_max_jerk_y);

    std::ostringstream gcode;
    gcode << "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=" << jerk;
    if (GCodeWriter::full_gcode_comment) gcode << " ; adjust jerk";
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_accel_and_jerk(unsigned int acceleration, double jerk)
{
    // Clamp the acceleration to the allowed maximum.
    if (EXTRUDER_LIMIT(m_max_acceleration) > 0 && acceleration > EXTRUDER_LIMIT(m_max_acceleration))
        acceleration = EXTRUDER_LIMIT(m_max_acceleration);
    
    bool is_empty = true;
    std::ostringstream gcode;
    gcode << "SET_VELOCITY_LIMIT";
    if (acceleration != 0 && acceleration != m_last_acceleration) {
        gcode << " ACCEL=" << acceleration;
        if (this->config.accel_to_decel_enable) {
            gcode << " MINIMUM_CRUISE_RATIO=" << (1.0 - this->config.accel_to_decel_factor / 100.0);
        }
        m_last_acceleration = acceleration;
        is_empty = false;
    }
    // Clamp the jerk to the allowed maximum.
    if (EXTRUDER_LIMIT(m_max_jerk_x) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_x))
        jerk = EXTRUDER_LIMIT(m_max_jerk_x);
    if (EXTRUDER_LIMIT(m_max_jerk_y) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_y))
        jerk = EXTRUDER_LIMIT(m_max_jerk_y);

    if (jerk > 0.01 && !is_approx(jerk, m_last_jerk)) {
        gcode << " SQUARE_CORNER_VELOCITY=" << jerk;
        m_last_jerk = jerk;
        is_empty = false;
    }

    if(is_empty)
        return std::string();

    if (GCodeWriter::full_gcode_comment)
        gcode << " ; adjust VELOCITY_LIMIT(accel/jerk)";
    gcode << "\n";

    return gcode.str();

}

std::string GCodeWriter::set_pressure_advance(double pa) const
{
    std::ostringstream gcode;
    if (pa < 0)
        return gcode.str();
    gcode << "SET_PRESSURE_ADVANCE ADVANCE=" << std::setprecision(4) << pa << "; Override pressure advance value\n";
    return gcode.str();
}

// Orca: input shaping support
std::string GCodeWriter::set_input_shaping(char axis, float damp, float freq, std::string type) const
{
    bool disable = type == "Disable";
    if (disable){
        freq = 0.0f;
        damp = 0.0f;
        axis = 'A';
        type = "Default";
    } else if (freq < 0.0f || damp < 0.f || damp > 1.0f || (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'A')) { // A = all axis
        throw std::runtime_error("Invalid input shaping parameters: axis=" + std::string(1, axis) + ", freq=" + std::to_string(freq) + ", damp=" + std::to_string(damp));
    }
    std::ostringstream gcode;
    std::ostringstream params;
    if (!type.empty() && type != "Default") {
        params << " SHAPER_TYPE=" << type;
    }
    if (axis != 'A') {
        if (freq > 0.0f) {
            params << " SHAPER_FREQ_" << axis << "=" << std::fixed << std::setprecision(2) << freq;
        }
        if (damp > 0.0f) {
            params << " DAMPING_RATIO_" << axis << "=" << std::fixed << std::setprecision(3) << damp;
        }
    } else {
        if (freq > 0.0f || disable) {
            params << " SHAPER_FREQ_X=" << std::fixed << std::setprecision(2) << freq << " SHAPER_FREQ_Y=" << std::fixed << std::setprecision(2) << freq;
        }
        if (damp > 0.0f || disable) {
            params << " DAMPING_RATIO_X=" << std::fixed << std::setprecision(3) << damp << " DAMPING_RATIO_Y=" << std::fixed << std::setprecision(3) << damp;
        }
    }
    if (!params.str().empty()) {
        gcode << "SET_INPUT_SHAPER" << params.str();
    }
    if (!gcode.str().empty()) {
        if (GCodeWriter::full_gcode_comment) {
            gcode << " ; Override input shaping";
        }
        gcode << "\n";
    }
    return gcode.str();
}


std::string GCodeWriter::reset_e(bool force)
{
    if (m_curr_extruder_id!=-1 && m_curr_filament_extruder[m_curr_extruder_id] != nullptr) {
        if (is_zero(m_curr_filament_extruder[m_curr_extruder_id]->E()) && ! force)
            return "";
        m_curr_filament_extruder[m_curr_extruder_id]->reset_E();
    }

    if (! this->config.use_relative_e_distances) {
        std::ostringstream gcode;
        gcode << "G92 E0";
        //BBS
        if (GCodeWriter::full_gcode_comment) gcode << " ; reset extrusion distance";
        gcode << "\n";
        return gcode.str();
    } else {
        return "";
    }
}

std::string GCodeWriter::toolchange_prefix() const
{
    // Orca: the manual-filament-change tag must stay ahead of the flavor selection so
    // MMU manual-change handling keeps working.
    if (config.manual_filament_change)
        return ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Manual_Tool_Change) + "T";
    return "T";
}

std::string GCodeWriter::toolchange(unsigned int filament_id, int nozzle_id)
{
    // set the new extruder
    auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
    assert(filament_extruder_iter != m_filament_extruders.end() && filament_extruder_iter->id() == filament_id);
    m_curr_extruder_id = filament_extruder_iter->extruder_id();
    m_curr_filament_extruder[m_curr_extruder_id] = &*filament_extruder_iter;
    m_cached_extruder_idx = get_extruder_index(this->config, filament_id);

    // return the toolchange command
    // if we are running a single-extruder setup, just set the extruder and return nothing
    std::ostringstream gcode;
    // Orca: also emit for single-extruder multi-filament setups (MMU-style).
    if (this->multiple_extruders || this->config.filament_diameter.values.size() > 1) {
        gcode << this->toolchange_prefix() << filament_id;
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; change extruder";
        gcode << "\n";
        gcode << this->reset_e(true);
    }
    return gcode.str();
}

// Current parked-retract length of the filament's extruder, share-aware. m_filament_extruders is
// sorted by id (see toolchange), so a lower_bound lookup finds the entry; unknown filament ids
// degrade to 0 rather than dereferencing end().
double GCodeWriter::get_extruder_retracted_length(const int filament_id)
{
    double res = 0.0;
    auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(),
        [filament_id](const Extruder &e) { return (int) e.id() < filament_id; });
    if (filament_extruder_iter == m_filament_extruders.end() || (int) filament_extruder_iter->id() != filament_id)
        return res;

    if (filament_extruder_iter->is_share_extruder())
        res = filament_extruder_iter->get_share_retracted_length();
    else
        res = filament_extruder_iter->get_single_retracted_length();

    return res;
}

std::string GCodeWriter::set_speed(double F, const std::string &comment, const std::string &cooling_marker)
{
    assert(F > 0.);
    assert(F < 100000.);
    
    m_current_speed = F;
    GCodeG1Formatter w;
    w.emit_f(F);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    w.emit_string(cooling_marker);
    return w.string();
}

std::string GCodeWriter::travel_to_xy(const Vec2d &point, const std::string &comment)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);

    this->set_current_position_clear(true);
    //BBS: take plate offset into consider
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG1Formatter w;
    w.emit_xy(point_on_plate);
    auto speed = m_is_first_layer
        ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx) : this->config.travel_speed.get_at(m_cached_extruder_idx);
    w.emit_f(speed * 60.0);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

/*  If this method is called more than once before calling unlift(),
it will not perform subsequent lifts, even if Z was raised manually
(i.e. with travel_to_z()) and thus _lifted was reduced. */
std::string GCodeWriter::lazy_lift(LiftType lift_type, bool spiral_vase)
{
    // check whether the above/below conditions are met
    double target_lift = 0;
    {
        //BBS
        int extruder_id = filament()->extruder_id();
        int filament_id = filament()->id();
        double above = this->config.retract_lift_above.get_at(extruder_id);
        double below = this->config.retract_lift_below.get_at(extruder_id);
        if (m_pos.z() >= above && (m_pos.z() <= below || below == 0.))
            target_lift = this->config.z_hop.get_at(filament_id);
    }
    // BBS
    if (m_lifted == 0 && m_to_lift == 0 && target_lift > 0) {
        if (spiral_vase) {
            m_lifted = target_lift;
            return this->_travel_to_z(m_pos(2) + target_lift, "lift Z");
        }
        else {
            m_to_lift = target_lift;
            m_to_lift_type = lift_type;
        }
    }
    return "";
}

// BBS: immediately execute an undelayed lift move with a spiral lift pattern
// designed specifically for subsequent gcode injection (e.g. timelapse) 
std::string GCodeWriter::eager_lift(const LiftType type) {
    std::string lift_move;
    double target_lift = 0;
    {
        //BBS
        int extruder_id = filament()->extruder_id();
        int filament_id = filament()->id();
        double above = this->config.retract_lift_above.get_at(extruder_id);
        double below = this->config.retract_lift_below.get_at(extruder_id);
        if (m_pos.z() >= above && (m_pos.z() <= below || below == 0.))
            target_lift = this->config.z_hop.get_at(filament_id);
    }

    // BBS: spiral lift only safe with known position
    if (type == LiftType::SpiralLift && this->is_current_position_clear()) {
        double radius = target_lift / (2 * PI * atan(filament()->travel_slope()));
        // static spiral alignment when no move in x,y plane.
        // spiral centra is a radius distance to the right (y=0)
        Vec2d ij_offset = { radius, 0 };
        // Orca: keep the spiral inside the active extruder's printable area, otherwise
        // fall back to a normal lift to avoid colliding with the print boundary. m_pos
        // includes the plate offset, so remove it to match the printable area coordinates.
        const Vec2d spiral_center = { m_pos.x() - m_x_offset + ij_offset.x(), m_pos.y() - m_y_offset + ij_offset.y() };
        if (target_lift > 0 && this->spiral_lift_fits_printable_area(spiral_center, radius)) {
            lift_move = this->_spiral_travel_to_z(m_pos(2) + target_lift, ij_offset, "spiral lift Z");
        } else if (target_lift > 0) {
            lift_move = _travel_to_z(m_pos(2) + target_lift, "normal lift Z");
        }
    }
    //BBS: if position is unknown use normal lift
    else if (target_lift > 0) {
        lift_move = _travel_to_z(m_pos(2) + target_lift, "normal lift Z");
    }
    m_lifted = target_lift;
    m_to_lift = 0;
    return lift_move;
}

std::string GCodeWriter::travel_to_xyz(const Vec3d &point, const std::string &comment, bool force_z)
{
    // FIXME: This function was not being used when travel_speed_z was separated (bd6badf).
    // Calculation of feedrate was not updated accordingly. If you want to use
    // this function, fix it first.
    //std::terminate();

    /*  If target Z is lower than current Z but higher than nominal Z we
        don't perform the Z move but we only move in the XY plane and
        adjust the nominal Z by reducing the lift amount that will be
        used for unlift. */
        // BBS
    Vec3d dest_point = point;
    auto travel_speed =
        m_is_first_layer ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx) : this->config.travel_speed.get_at(m_cached_extruder_idx);
    //BBS: a z_hop need to be handle when travel
    if (std::abs(m_to_lift) > EPSILON) {
        assert(std::abs(m_lifted) < EPSILON);
        //BBS: don't need to do real lift if the current position is absolutely same with target.
        //This ususally happens when the last extrusion line is short and the end of wipe position
        //is same with the traget point by chance.
        if ((!this->is_current_position_clear() || m_pos != dest_point) &&
            m_to_lift + m_pos(2) > point(2)) {
            m_lifted = m_to_lift + m_pos(2) - point(2);
            dest_point(2) = m_to_lift + m_pos(2);
        }
        m_to_lift = 0.;

        std::string slop_move;
        //BBS: minus plate offset
        Vec3d source = { m_pos(0) - m_x_offset, m_pos(1) - m_y_offset, m_pos(2) };
        Vec3d target = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
        Vec3d delta = target - source;
        Vec2d delta_no_z = { delta(0), delta(1) };
        //BBS: don'need slope travel because we don't know where is the source position the first time
        //BBS: Also don't need to do slope move or spiral lift if x-y distance is absolute zero
        if (delta(2) > 0 && delta_no_z.norm() != 0.0f)    {
            //BBS: SpiralLift
            if (m_to_lift_type == LiftType::SpiralLift && this->is_current_position_clear()) {
                //BBS: todo: check the arc move all in bed area, if not, then use lazy lift
                double radius = delta(2) / (2 * PI * atan(this->filament()->travel_slope()));
                Vec2d ij_offset = radius * delta_no_z.normalized();
                ij_offset = { -ij_offset(1), ij_offset(0) };
                // Orca: only perform the spiral lift if its full circle stays inside the
                // printable area of the active extruder, otherwise fall back to a normal
                // lift to avoid colliding with the print boundary. `source` is already in
                // bed coordinates (plate offset removed), matching the printable area.
                const Vec2d spiral_center = { source.x() + ij_offset.x(), source.y() + ij_offset.y() };
                if (this->spiral_lift_fits_printable_area(spiral_center, radius))
                    slop_move = this->_spiral_travel_to_z(target(2), ij_offset, "spiral lift Z");
                else
                    slop_move = _travel_to_z(target.z(), "normal lift Z");
            }
            //BBS: SlopeLift
            else if (m_to_lift_type == LiftType::SlopeLift &&
                this->is_current_position_clear() &&
                atan2(delta(2), delta_no_z.norm()) < this->filament()->travel_slope()) {
                //BBS: check whether we can make a travel like
                //   _____
                //  /       to make the z list early to avoid to hit some warping place when travel is long.
                Vec2d temp = delta_no_z.normalized() * delta(2) / tan(this->filament()->travel_slope());
                Vec3d slope_top_point = Vec3d(temp(0), temp(1), delta(2)) + source;
                GCodeG1Formatter w0;
                w0.emit_xyz(slope_top_point);
                w0.emit_f(travel_speed * 60.0);
                //BBS
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                slop_move = w0.string();
            }
            else if (m_to_lift_type == LiftType::NormalLift) {
                slop_move = _travel_to_z(target.z(), "normal lift Z");
            }
        }

        std::string xy_z_move;
        {
            GCodeG1Formatter w0;
            if (this->is_current_position_clear()) {
                w0.emit_xyz(target);
                w0.emit_f(travel_speed * 60.0);
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                xy_z_move = w0.string();
            }
            else {
                w0.emit_xy(Vec2d(target.x(), target.y()));
                w0.emit_f(travel_speed * 60.0);
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                xy_z_move = w0.string() + _travel_to_z(target.z(), comment);
            }
        }
        m_pos = dest_point;
        this->set_current_position_clear(true);
        return slop_move + xy_z_move;
    }
    else if (!force_z && !this->will_move_z(point(2))) {
        double nominal_z = m_pos(2) - m_lifted;
        m_lifted -= (point(2) - nominal_z);
        // In case that z_hop == layer_height we could end up with almost zero in_m_lifted
        // and a retract could be skipped
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        //BBS
        this->set_current_position_clear(true);
        return this->travel_to_xy(to_2d(point));
    }
    else {
        /*  In all the other cases, we perform an actual XYZ move and cancel
            the lift. */
        m_lifted = 0;
    }

    //BBS: take plate offset into consider
    Vec3d point_on_plate = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
    std::string out_string;
    GCodeG1Formatter w;
    if (!this->is_current_position_clear())
    {
        //force to move xy first then z after filament change
        w.emit_xy(Vec2d(point_on_plate.x(), point_on_plate.y()));
        w.emit_f(this->config.travel_speed.get_at(m_cached_extruder_idx) * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string() + _travel_to_z(point_on_plate.z(), comment);
    } else {
        GCodeG1Formatter w;
        w.emit_xyz(point_on_plate);
        w.emit_f(this->config.travel_speed.get_at(m_cached_extruder_idx) * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string();
    }

    m_pos = dest_point;
    this->set_current_position_clear(true);
    return out_string;
}

std::string GCodeWriter::travel_to_z(double z, const std::string &comment, bool force)
{
    /*  If target Z is lower than current Z but higher than nominal Z
        we don't perform the move but we only adjust the nominal Z by
        reducing the lift amount that will be used for unlift. */
    if (!force && !this->will_move_z(z)) {
        double nominal_z = m_pos(2) - m_lifted;
        m_lifted -= (z - nominal_z);
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        return "";
    }

    /*  In all the other cases, we perform an actual Z move and cancel
        the lift. */
    m_lifted = 0;
    return this->_travel_to_z(z, comment);
}

std::string GCodeWriter::_travel_to_z(double z, const std::string &comment)
{
    m_pos(2) = z;

    double speed = this->config.travel_speed_z.get_at(m_cached_extruder_idx);
    if (speed == 0.) {
        speed = m_is_first_layer ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
                                 : this->config.travel_speed.get_at(m_cached_extruder_idx);
    }

    GCodeG1Formatter w;
    w.emit_z(z);
    w.emit_f(speed * 60.0);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::_spiral_travel_to_z(double z, const Vec2d &ij_offset, const std::string &comment)
{
    std::string output;
    double speed = this->config.travel_speed_z.get_at(m_cached_extruder_idx);

    if (speed == 0.) {
        speed = m_is_first_layer ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
                                 : this->config.travel_speed.get_at(m_cached_extruder_idx);
    }

    if (!this->config.enable_arc_fitting) { // Orca: if arc fitting is disabled, approximate the arc with small linear segments
        const double z_start = m_pos(2); // starting Z height

        const double px = m_pos(0) - m_x_offset;        // take plate offset into consideration
        const double py = m_pos(1) - m_y_offset;        // take plate offset into consideration
        const double cx = px + ij_offset(0);            // center x
        const double cy = py + ij_offset(1);            // center y
        const double radius = ij_offset.norm();         // radius

        // Number of linear segments approximating the circle, chosen so that a chord never deviates
        // from the true arc by more than the slicing resolution. A resolution of 0 means "no
        // simplification", which has no finite segment count, so it takes the upper bound.
        constexpr size_t min_segments = 8;              // keep a small spiral visibly round
        constexpr size_t max_segments = 128;            // bound the emitted G-code
        const int segments = int(m_resolution > 0. ?
            std::clamp(Geometry::ArcWelder::arc_discretization_steps(radius, 2. * M_PI, m_resolution), min_segments, max_segments) :
            max_segments);

        const double a0 = std::atan2(py - cy, px - cx); // start angle

        auto emit_point = [&output](const Vec3d &point) {
            GCodeG1Formatter w;
            w.emit_xyz(point);
            output += w.string();
        };

        output.reserve(size_t(segments) * 40);          // ~40 characters per emitted G1 line

        GCodeG1Formatter w;                             // set feedrate
        w.emit_f(speed * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        output += w.string();

        // approximate the arc with small linear segments (without the last point which is added later to ensure exactness)
        for (int i = 1; i < segments; ++i) {
            const double t = double(i) / segments;      // parametric position along arc
            const double a = a0 + 2. * M_PI * t;        // CCW arc param, full circle
            emit_point(Vec3d(cx + radius * std::cos(a), // point on circle
                             cy + radius * std::sin(a),
                             z_start + (z - z_start) * t)); // interpolated Z height
        }

        emit_point(Vec3d(px, py, z));                   // final point to ensure exactness
    } else { // Orca: if arc fitting is enabled emit a G2/G3 command for the spiral lift
        output = std::string("G17") + (full_gcode_comment ? " ; XY plane for arc\n" : "\n");

        GCodeG2G3Formatter w(true);
        w.emit_z(z);
        w.emit_ij(ij_offset);
        w.emit_string(" P1 ");
        w.emit_f(speed * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);

        output += w.string();
    }

    m_pos(2) = z;
    return output;
}

bool GCodeWriter::will_move_z(double z) const
{
    /* If target Z is lower than current Z but higher than nominal Z
        we don't perform an actual Z move. */
    if (m_lifted > 0) {
        double nominal_z = m_pos(2) - m_lifted;
        if (z >= nominal_z && z <= m_pos(2))
            return false;
    }
    // BBS.
    // Dont move z if it is the same as target z
    else if (std::abs(m_pos(2) - z) < EPSILON) {
        return false;
    }
    return true;
}

std::string GCodeWriter::extrude_to_xy(const Vec2d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if(std::abs(dE) <= std::numeric_limits<double>::epsilon())
        force_no_extrusion = true;

    if (!force_no_extrusion)
        filament()->extrude(dE);

    //BBS: take plate offset into consider
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG1Formatter w;
    w.emit_xy(point_on_plate);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

//BBS: generate G2 or G3 extrude which moves by arc
//point is end point which means X and Y axis
//center_offset is I and J axis
std::string GCodeWriter::extrude_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string& comment, bool force_no_extrusion)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if (!force_no_extrusion)
        filament()->extrude(dE);

    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG2G3Formatter w(is_ccw);
    w.emit_xy(point_on_plate);
    w.emit_ij(center_offset);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    // Check if Z actually changes (at export precision) before emitting it.
    // ZAA sloped extrusions call this for every segment, but many consecutive
    // segments share the same quantized Z — emitting it every time is redundant.
    bool z_changed = (GCodeG1Formatter::quantize_xyzf(point(2)) != GCodeG1Formatter::quantize_xyzf(m_pos(2)));

    m_pos = point;
    m_lifted = 0;
    if (!force_no_extrusion)
        filament()->extrude(dE);

    //BBS: take plate offset into consider
    Vec3d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset, point(2) };

    GCodeG1Formatter w;
    if (z_changed)
        w.emit_xyz(point_on_plate);
    else
        w.emit_xy(Vec2d(point_on_plate.x(), point_on_plate.y()));
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::retract(bool before_wipe, double retract_length)
{
    double factor = before_wipe ? filament()->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    return this->_retract(
        retract_length > EPSILON ? retract_length : factor * filament()->retraction_length(),
        factor * filament()->retract_restart_extra(),
        "retract"
    );
}

std::string GCodeWriter::retract_for_toolchange(bool before_wipe, double retract_length)
{
    double factor = before_wipe ? filament()->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    return this->_retract(
        retract_length > EPSILON ? retract_length : factor * filament()->retract_length_toolchange(),
        factor * filament()->retract_restart_extra_toolchange(),
        "retract for toolchange"
    );
}

std::string GCodeWriter::_retract(double length, double restart_extra, const std::string &comment)
{
    /*  If firmware retraction is enabled, we use a fake value of 1
    since we ignore the actual configured retract_length which
    might be 0, in which case the retraction logic gets skipped. */
    if (this->config.use_firmware_retraction)
        length = 1;

    std::string gcode;
    if (double dE = filament()->retract(length, restart_extra);  !is_zero(dE)) {
        if (this->config.use_firmware_retraction) {
            gcode = this->set_firmware_retraction();
            gcode += "G10 ; retract\n";
            m_firmware_retracted = true;
        }
        else {
            // BBS
            GCodeG1Formatter w;
            w.emit_e(filament()->E());
            w.emit_f(filament()->retract_speed() * 60.);
            // BBS
            w.emit_comment(GCodeWriter::full_gcode_comment, comment);
            gcode = w.string();
        }
    }

    return gcode;
}

std::string GCodeWriter::set_firmware_retraction()
{
    // Klipper's SET_RETRACTION also clears the [firmware_retraction] "retracted" flag, so a G11 issued
    // after it does nothing. The flag is global, while the writer keeps a retraction state per
    // extruder: after a tool change the new tool may retract while the parked one still holds the
    // flag. Hold the values back until a G11 has cleared it.
    const Extruder *fil = filament();
    if (fil == nullptr || !this->config.use_firmware_retraction || this->config.firmware_owns_retraction || m_firmware_retracted)
        return {};
    const std::array<double, 4> values{ fil->retraction_length(), double(fil->retract_speed()),
                                        fil->retract_restart_extra(), double(fil->deretract_speed()) };
    if (m_firmware_retraction_sent == values)
        return {};
    m_firmware_retraction_sent = values;

    std::ostringstream gcode;
    gcode << "SET_RETRACTION RETRACT_LENGTH=" << values[0] << " RETRACT_SPEED=" << values[1]
          << " UNRETRACT_EXTRA_LENGTH=" << values[2] << " UNRETRACT_SPEED=" << values[3];
    if (GCodeWriter::full_gcode_comment)
        gcode << " ; retraction of the active filament";
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::unretract(float extra_retract)
{
    std::string gcode;

    if (double dE = filament()->unretract(); !is_zero(dE)) {
        if (this->config.use_firmware_retraction) {
            gcode += "G11 ; unretract\n";
            m_firmware_retracted = false;
            gcode += this->reset_e();
            // G11 restored what the last G10 retracted with the values active at that time. Only now
            // may the values of a newly activated filament replace them.
            gcode += this->set_firmware_retraction();
        }
        else {
            //BBS
            // use G1 instead of G0 because G0 will blend the restart with the previous travel move
            GCodeG1Formatter w;
            // extra_retract over-extrudes for the PETG pre-extrusion; 0 by
            // default -> identical to the plain deretract E position.
            w.emit_e(filament()->E() + extra_retract);
            w.emit_f(filament()->deretract_speed() * 60.);
            //BBS
            w.emit_comment(GCodeWriter::full_gcode_comment, " ; unretract");
            gcode += w.string();
        }
    }

    return gcode;
}


std::string GCodeWriter::unlift()
{
    std::string gcode;
    if (m_lifted > 0) {
        gcode += this->_travel_to_z(m_pos(2) - m_lifted, "restore layer Z");
        m_lifted = 0;
    }
    m_to_lift = 0.;
    return gcode;
}

std::string GCodeWriter::format_fan(unsigned int speed, unsigned int part_cooling_fan_min_pwm)
{
    std::ostringstream gcode;
    // ORCA: clamp non-zero fan commands up to the configured PWM floor so fans that can't spool at low duty
    // cycles still start reliably. Zero (fan off) is preserved exactly so disable-fan commands are never altered.
    if (speed > 0 && part_cooling_fan_min_pwm > 0 && speed < part_cooling_fan_min_pwm)
        speed = part_cooling_fan_min_pwm;
    if (speed == 0) {
        gcode << "M106 S0";
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; disable fan";
        gcode << "\n";
    } else {
        gcode << "M106 S" << static_cast<unsigned int>(255.5 * speed / 100.0);
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; enable fan";
        gcode << "\n";
    }
    return gcode.str();
}

std::string GCodeWriter::set_fan(unsigned int speed) const
{
    //BBS
    // ORCA: pick up the per-printer PWM floor from the active config.
    return GCodeWriter::format_fan(speed, static_cast<unsigned int>(std::max(0, this->config.part_cooling_fan_min_pwm.value)));
}

//BBS: set additional fan speed for BBS machine only
std::string GCodeWriter::set_additional_fan(unsigned int speed)
{
    std::ostringstream gcode;

    gcode << "M106 " << "P2 " << "S" << (int)(255.0 * speed / 100.0);
    if (GCodeWriter::full_gcode_comment) {
        if (speed == 0)
            gcode << " ; disable additional fan ";
        else
            gcode << " ; enable additional fan ";
    }
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_exhaust_fan(int speed)
{
    std::ostringstream gcode;
    gcode << "M106" << " P3" << " S" << (int)(speed / 100.0 * 255);

    if (GCodeWriter::full_gcode_comment) {
        if (speed == 0)
            gcode << " ; disable exhaust fan ";
        else
            gcode << " ; enable exhaust fan ";
    }

    gcode << "\n";
    return gcode.str();
}

void GCodeWriter::add_object_start_labels(std::string& gcode)
{
    if (!m_gcode_label_objects_start.empty()) {
        gcode += m_gcode_label_objects_start;
        m_gcode_label_objects_start = "";
    }
}

void GCodeWriter::add_object_end_labels(std::string& gcode)
{
    if (!m_gcode_label_objects_end.empty()) {
        gcode += m_gcode_label_objects_end;
        m_gcode_label_objects_end = "";

        // Orca: reset E so that e value remain correct after skipping the object
        // ref to: https://github.com/OrcaSlicer/OrcaSlicer/pull/205/commits/7f1fe0bd544077626080aa1a9a0576aa735da1a4#r1083470162
        if (!this->config.use_relative_e_distances)
            gcode += reset_e(true);
    }
}

void GCodeWriter::add_object_change_labels(std::string& gcode)
{
    add_object_end_labels(gcode);
    add_object_start_labels(gcode);
}

std::string GCodeWriter::set_extruder(unsigned int filament_id)
{
    auto filament_ext_it = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
    unsigned int extruder_id = filament_ext_it->extruder_id();
    assert(filament_ext_it != m_filament_extruders.end() && filament_ext_it->id() == filament_id);
    // Orca: writer-only context (calibration paths) has no nozzle grouping; the
    // filament's own extruder id is the correct degenerate nozzle value.
    return this->need_toolchange(filament_id) ? this->toolchange(filament_id, (int) extruder_id) : "";
}

void GCodeWriter::init_extruder(unsigned int filament_id)
{
    if (m_curr_extruder_id == -1 && filament_id != -1) {
        auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
        assert(filament_extruder_iter != m_filament_extruders.end() && filament_extruder_iter->id() == filament_id);
        m_curr_extruder_id = filament_extruder_iter->extruder_id();
        m_curr_filament_extruder[m_curr_extruder_id] = &*filament_extruder_iter;
        m_cached_extruder_idx = get_extruder_index(this->config, filament_id);
    }
}

bool GCodeWriter::need_toolchange(unsigned int filament_id)const
{
    return filament()==nullptr || filament()->id()!=filament_id;
}

void GCodeFormatter::emit_axis(const char axis, const double v, size_t digits) {
    assert(digits <= 9);
    static constexpr const std::array<int, 10> pow_10{1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    *ptr_err.ptr++ = ' '; *ptr_err.ptr++ = axis;

    char *base_ptr = this->ptr_err.ptr;
    auto  v_int    = int64_t(std::round(v * pow_10[digits]));
    // Older stdlib on macOS doesn't support std::from_chars at all, so it is used boost::spirit::karma::generate instead of it.
    // That is a little bit slower than std::to_chars but not much.
#ifdef __APPLE__
    boost::spirit::karma::generate(this->ptr_err.ptr, boost::spirit::karma::int_generator<int64_t>(), v_int);
#else
    // this->buf_end minus 1 because we need space for adding the extra decimal point.
    this->ptr_err = std::to_chars(this->ptr_err.ptr, this->buf_end - 1, v_int);
#endif
    size_t writen_digits = (this->ptr_err.ptr - base_ptr) - (v_int < 0 ? 1 : 0);
    if (writen_digits < digits) {
        // Number is smaller than 10^digits, so that we will pad it with zeros.
        size_t remaining_digits = digits - writen_digits;
        // Move all newly inserted chars by remaining_digits to allocate space for padding with zeros.
        for (char *from_ptr = this->ptr_err.ptr - 1, *to_ptr = from_ptr + remaining_digits; from_ptr >= this->ptr_err.ptr - writen_digits; --to_ptr, --from_ptr)
            *to_ptr = *from_ptr;

        memset(this->ptr_err.ptr - writen_digits, '0', remaining_digits);
        this->ptr_err.ptr += remaining_digits;
    }

    // Move all newly inserted chars by one to allocate space for a decimal point.
    for (char *to_ptr = this->ptr_err.ptr, *from_ptr = to_ptr - 1; from_ptr >= this->ptr_err.ptr - digits; --to_ptr, --from_ptr)
        *to_ptr = *from_ptr;

    *(this->ptr_err.ptr - digits) = '.';
    for (size_t i = 0; i < digits; ++i) {
        if (*this->ptr_err.ptr != '0')
            break;
        this->ptr_err.ptr--;
    }
    if (*this->ptr_err.ptr == '.')
        this->ptr_err.ptr--;
    if ((this->ptr_err.ptr + 1) == base_ptr || *this->ptr_err.ptr == '-')
        *(++this->ptr_err.ptr) = '0';
    this->ptr_err.ptr++;

#if 0 // #ifndef NDEBUG
    {
        // Verify that the optimized formatter produces the same result as the standard sprintf().
        double v1 = atof(std::string(base_ptr, this->ptr_err.ptr).c_str());
        char buf[2048];
        sprintf(buf, "%.*lf", int(digits), v);
        double v2 = atof(buf);
        // Numbers may differ when rounding at exactly or very close to 0.5 due to numerical issues when scaling the double to an integer.
        // Thus the complex assert.
//        assert(v1 == v2);
        assert(std::abs(v1 - v) * pow_10[digits] < 0.50001);
        assert(std::abs(v2 - v) * pow_10[digits] < 0.50001);
    }
#endif // NDEBUG
}

} // namespace Slic3r
