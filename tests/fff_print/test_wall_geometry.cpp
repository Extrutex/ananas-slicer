// Wall-geometry oracle.
//
// Slices a parametric test plate in-process and measures the wall ExtrusionPaths of one layer against the
// rounded-rectangle bead model that Flow uses: a bead of width w and height h is a rectangle with
// semicircular sides, so its cross-section is h * (w - c) with c = h * (1 - pi/4), and its spacing is
// s = w - c. Every band below is derived from that model and from the settings this file pins, never from
// the slicer's current output.
//
// The plate holds:
//   - fins of 0.15 to 1.2 mm, each its own island;
//   - a 10 mm block (outer bead position of a full multi-bead wall, one run per loop);
//   - a wedge whose thickness runs from 0.05 to 2.0 mm (thin-feature cutoff, chaining);
//   - a plate with round holes of 0.4 to 3.0 mm (clear hole diameter).
//
// Checks:
//   (a) presence: every fin at least as thick as the generator's documented minimum is printed; on the
//       wedge, the last extruded point lies where the model is no thicker than that minimum + 0.05 mm.
//   (b) volume: deposited cross-section over the middle of a fin lies between the dimension-exact beads
//       (outer bead edges on the model surface, neighbours overlapping by c) and the volume-exact beads
//       (total spacing = thickness), never below the generator's widening floor.
//   (c) single bead: it is centred on the fin. Arachne's width is criterion Q1 of the wall analysis,
//       max(t, widened floor) +- 0.005 mm; classic's lies in the volume band of (b).
//   (c') precise parity: an Arachne single bead is as wide with precise_outer_wall on as with it off,
//       +- 0.005 mm (the second half of Q1). This compares two cells, so it is its own test case.
//   (d) outer bead: its centerline is w/2 from the model edge, i.e. its outer edge is on the surface.
//   (e) holes: the clear diameter of every hole is the model diameter within the slice resolution.
//   (f) runs: n beads across a feature need at most ceil(n/2) wall runs (loops pair the beads, an odd
//       middle bead is one open line), so more runs means wall pieces that are not chained.
//   (g) method parity, outline_shrink vs toolpath_shift: on the block every bead and the infill region sit
//       where the other method puts them, and no wall on the plate is narrower than the narrowest wall of
//       outline_shrink (criterion Q7).
//
// Matrix: {arachne, classic, classic + detect_thin_wall} x precise_outer_wall {0, 1}, plus Arachne with
// precise_outer_wall_method = toolpath_shift.
//
// Known defects are listed in known_failures(). A cell test case requires each listed check to still
// disagree with the bead model and every other check to agree, so a fix shows up check by check. Each
// defect also has a [!shouldfail] test case that asserts its checks as normal checks: it stays green while
// the defect exists and turns red once it is fixed. The fixing commit removes the defect's entries and
// the [!shouldfail] tag. The hidden [WallGeometryReport] case prints every measurement of every cell.

#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// ---- Settings every band is derived from (all pinned in cell_config()) ----

constexpr double nozzle_diameter = 0.4;
constexpr double layer_height    = 0.2;
constexpr double wall_width      = 0.4;   // outer and inner wall line width
constexpr int    wall_loops      = 3;
constexpr double resolution      = 0.012; // slice simplification

constexpr double min_feature_size = 0.25 * nozzle_diameter; // min_feature_size 25%
// Arachne widens a bead thinner than min_bead_width up to it. Arachne works in the spacing domain
// (VariableWidth.cpp turns a spacing into an extrusion width by adding c), so the widened bead is
// min_bead_width + c wide.
constexpr double min_bead_width = 0.85 * nozzle_diameter;   // min_bead_width 85%
// Classic builds no thin wall narrower than a third of the nozzle (PerimeterGenerator::process_classic).
constexpr double classic_thin_wall_min = nozzle_diameter / 3.;

// Width minus spacing of a bead.
const double corner = layer_height * (1. - 0.25 * PI);

// Tolerance on one bead's width or position (criterion Q1 of the wall analysis).
constexpr double bead_tolerance = 0.005;

// Target width of an Arachne single bead. The wall analysis is not consistent here: Q1 asks for
// max(t, floor) +- 0.005 mm, while the WALL-1 fix it plans (Option B: beading on the true thickness) and its
// geometric reference (classic + detect_thin_wall) put the bead on spacing t, i.e. width t + c. The oracle
// follows the written criterion, Q1. Switching to the band [max(t, floor), max(t + c, floor)] needs an
// explicit decision recorded in the analysis before the WALL-1 fix, not after it. The precise parity check
// below holds either way.
enum class ArachneSingleBead { q1_model_thickness, bead_model_band };
constexpr ArachneSingleBead arachne_single_bead = ArachneSingleBead::q1_model_thickness;

// Allowance on the wedge cutoff (criterion Q2: cutoff <= minimum feature + 0.05 mm).
constexpr double wedge_cutoff_allowance = 0.05;

// ---- Test plate (mm, model frame) ----

constexpr double model_height = 2.0;
constexpr double probe_z      = 1.0; // layer 5 of 10, clear of the 3 bottom and 3 top shell layers

const std::vector<double> fin_thicknesses = { 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2 };
constexpr double fin_length = 12.;
constexpr double fin_gap    = 3.;
// The volume of a fin is integrated over its middle, away from the bead ends.
constexpr double fin_window_margin = 3.;

constexpr double block_x0 = 0., block_y0 = 16., block_size = 10.;

constexpr double wedge_x0 = 14., wedge_length = 40., wedge_y = 21.;
constexpr double wedge_thin = 0.05, wedge_thick = 2.0;

const std::vector<double> hole_diameters = { 0.4, 0.6, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0 };
constexpr double plate_y0 = 30., hole_cell = 8.;
constexpr int    hole_segments = 64;

// Every feature's runs are attributed by where they start, within this margin around the feature.
constexpr double attribution_margin = 1.;

struct Rect { double x0, y0, x1, y1; };

std::vector<Rect> fin_rects()
{
    std::vector<Rect> out;
    double x = 0.;
    for (double t : fin_thicknesses) {
        out.push_back({ x, 0., x + t, fin_length });
        x += t + fin_gap;
    }
    return out;
}

Vec2d hole_center(size_t i) { return { hole_cell * (double(i) + 0.5), plate_y0 + 0.5 * hole_cell }; }

double wedge_thickness_at(double x) { return wedge_thin + (wedge_thick - wedge_thin) * (x - wedge_x0) / wedge_length; }

Polygon polygon_mm(const std::vector<Vec2d> &pts)
{
    Polygon out;
    for (const Vec2d &p : pts)
        out.points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    return out;
}

Polygon rect_polygon(const Rect &r) { return polygon_mm({ { r.x0, r.y0 }, { r.x1, r.y0 }, { r.x1, r.y1 }, { r.x0, r.y1 } }); }

ExPolygons plate_shapes()
{
    ExPolygons out;
    for (const Rect &r : fin_rects())
        out.emplace_back(rect_polygon(r));
    out.emplace_back(rect_polygon({ block_x0, block_y0, block_x0 + block_size, block_y0 + block_size }));
    out.emplace_back(polygon_mm({ { wedge_x0, wedge_y - 0.5 * wedge_thin }, { wedge_x0 + wedge_length, wedge_y - 0.5 * wedge_thick },
                                  { wedge_x0 + wedge_length, wedge_y + 0.5 * wedge_thick }, { wedge_x0, wedge_y + 0.5 * wedge_thin } }));
    ExPolygon plate(rect_polygon({ 0., plate_y0, hole_cell * double(hole_diameters.size()), plate_y0 + hole_cell }));
    for (size_t i = 0; i < hole_diameters.size(); ++ i) {
        std::vector<Vec2d> pts;
        for (int k = 0; k < hole_segments; ++ k) {
            const double a = 2. * PI * k / hole_segments;
            pts.push_back(hole_center(i) + 0.5 * hole_diameters[i] * Vec2d(std::cos(a), -std::sin(a))); // clockwise
        }
        plate.holes.emplace_back(polygon_mm(pts));
    }
    out.emplace_back(std::move(plate));
    return out;
}

// A closed prism of `height` over each shape. Contours run counter-clockwise and holes clockwise, so every
// side quad faces out of the solid.
TriangleMesh extrude(const ExPolygons &shapes, double height)
{
    indexed_triangle_set its;
    auto add = [&its](const Vec3d &a, const Vec3d &b, const Vec3d &c) {
        const int i = int(its.vertices.size());
        its.vertices.emplace_back(a.cast<float>());
        its.vertices.emplace_back(b.cast<float>());
        its.vertices.emplace_back(c.cast<float>());
        its.indices.emplace_back(i, i + 1, i + 2);
    };
    for (const ExPolygon &shape : shapes) {
        for (bool top : { false, true }) {
            const std::vector<Vec3d> tris = triangulate_expolygon_3d(shape, top ? height : 0., top ? NORMALS_UP : !NORMALS_UP);
            for (size_t i = 0; i + 2 < tris.size(); i += 3)
                add(tris[i], tris[i + 1], tris[i + 2]);
        }
        for (const Polygon &poly : to_polygons(shape))
            for (size_t i = 0; i < poly.points.size(); ++ i) {
                const Vec2d a = unscaled(poly.points[i]);
                const Vec2d b = unscaled(poly.points[(i + 1) % poly.points.size()]);
                add({ a.x(), a.y(), 0. }, { b.x(), b.y(), 0. }, { b.x(), b.y(), height });
                add({ a.x(), a.y(), 0. }, { b.x(), b.y(), height }, { a.x(), a.y(), height });
            }
    }
    its_merge_vertices(its);
    return TriangleMesh(std::move(its));
}

// ---- Matrix ----

struct Cell {
    const char *name;
    const char *generator;
    bool        thin_walls;
    bool        precise;
    // precise_outer_wall_method
    const char *method = "outline_shrink";
};

const Cell cells[] = {
    { "arachne/precise=0",      "arachne", false, false },
    { "arachne/precise=1",      "arachne", false, true  },
    { "arachne/precise=1/shift", "arachne", false, true, "toolpath_shift" },
    { "classic/precise=0",      "classic", false, false },
    { "classic/precise=1",      "classic", false, true  },
    { "classic+thin/precise=0", "classic", true,  false },
    { "classic+thin/precise=1", "classic", true,  true  },
};

const Cell &cell_named(const std::string &name)
{
    for (const Cell &c : cells)
        if (name == c.name)
            return c;
    throw std::invalid_argument("unknown wall geometry cell " + name);
}

bool is_arachne(const Cell &cell) { return std::string(cell.generator) == "arachne"; }

DynamicPrintConfig cell_config(const Cell &cell)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",                cell.generator },
        { "detect_thin_wall",              cell.thin_walls },
        { "precise_outer_wall",            cell.precise },
        { "precise_outer_wall_method",     cell.method },
        // precise_outer_wall only acts with the inner walls printed first.
        { "wall_sequence",                 "inner wall/outer wall" },
        { "nozzle_diameter",               "0.4" },
        { "layer_height",                  layer_height },
        { "initial_layer_print_height",    layer_height },
        { "line_width",                    wall_width },
        { "outer_wall_line_width",         wall_width },
        { "inner_wall_line_width",         wall_width },
        { "initial_layer_line_width",      wall_width },
        { "wall_loops",                    wall_loops },
        { "min_feature_size",              "25%" },
        { "min_bead_width",                "85%" },
        { "resolution",                    resolution },
        { "top_shell_layers",              3 },
        { "bottom_shell_layers",           3 },
        { "sparse_infill_density",         "15%" },
        { "xy_hole_compensation",          0. },
        { "xy_contour_compensation",       0. },
        { "elefant_foot_compensation",     0. },
        { "only_one_wall_top",             false },
        { "only_one_wall_first_layer",     false },
        { "alternate_extra_wall",          false },
        { "extra_perimeters_on_overhangs", false },
        { "fuzzy_skin",                    "none" },
        { "enable_arc_fitting",            false },
        { "spiral_mode",                   false },
    });
    return config;
}

// ---- The extrusions of the probe layer ----

struct Bead {
    Polyline polyline;
    double   width;
    double   mm3_per_mm;
    size_t   run;  // the loop, multipath or single path this piece is extruded in
    bool     wall; // from the perimeters, not the fills (gap fill, infill)
};

void collect(const ExtrusionEntity *entity, bool wall, size_t &run, std::vector<Bead> &out)
{
    auto add_run = [&](const ExtrusionPaths &paths) {
        for (const ExtrusionPath &p : paths)
            out.push_back({ p.polyline.to_polyline(), double(p.width), p.mm3_per_mm, run, wall });
        ++ run;
    };
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect(child, wall, run, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        add_run(loop->paths);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        add_run(multi->paths);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        add_run(ExtrusionPaths{ *path });
    }
}

struct ProbeLayer {
    std::vector<Bead> beads;
    ExPolygons        fill_area; // the region left for infill inside the walls
    Vec2d             offset;    // model frame + offset = layer frame
};

ProbeLayer slice_probe_layer(const Cell &cell)
{
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(extrude(plate_shapes(), model_height));
    init_print(std::move(meshes), print, model, cell_config(cell));
    print.process();

    REQUIRE(print.objects().size() == 1);
    const Layer *probe = nullptr;
    for (const Layer *layer : print.objects().front()->layers())
        if (std::abs(layer->print_z - probe_z) < EPSILON)
            probe = layer;
    REQUIRE(probe != nullptr);

    ProbeLayer out;
    // Slicing is exact, so the lower left corner of the slices is the model's (0, 0).
    out.offset = unscaled(get_extents(probe->lslices).min);
    size_t run = 0;
    for (const LayerRegion *region : probe->regions()) {
        collect(&region->perimeters, true, run, out.beads);
        collect(&region->fills, false, run, out.beads);
        append(out.fill_area, region->fill_expolygons);
    }
    return out;
}

// ---- Checks ----

constexpr double unbounded = std::numeric_limits<double>::infinity();

struct Check {
    std::string feature;
    std::string kind;
    double      measured;
    double      lo;
    double      hi;
    std::string detail;

    bool pass() const { return measured >= lo - 1e-9 && measured <= hi + 1e-9; }
    std::string describe() const
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%-9s %-14s %8.4f in [%.4f, %.4f] %s", feature.c_str(), kind.c_str(), measured, lo, hi,
                      pass() ? "ok" : "FAIL");
        return detail.empty() ? std::string(buf) : std::string(buf) + "  " + detail;
    }
};

// The beads of every run that starts inside `box` (layer frame). A run belongs to one feature as a whole.
std::vector<const Bead *> beads_in(const ProbeLayer &layer, const BoundingBoxf &box, bool walls_only = false)
{
    std::vector<size_t> runs;
    for (const Bead &b : layer.beads)
        if (! b.polyline.points.empty() && box.contains(unscaled(b.polyline.points.front())) &&
            std::find(runs.begin(), runs.end(), b.run) == runs.end())
            runs.push_back(b.run);
    std::vector<const Bead *> out;
    for (const Bead &b : layer.beads)
        if ((b.wall || ! walls_only) && std::find(runs.begin(), runs.end(), b.run) != runs.end())
            out.push_back(&b);
    return out;
}

BoundingBoxf around(const Rect &r)
{
    return BoundingBoxf(Vec2d(r.x0 - attribution_margin, r.y0 - attribution_margin), Vec2d(r.x1 + attribution_margin, r.y1 + attribution_margin));
}

size_t wall_runs(const std::vector<const Bead *> &beads)
{
    std::vector<size_t> runs;
    for (const Bead *b : beads)
        if (b->wall && std::find(runs.begin(), runs.end(), b->run) == runs.end())
            runs.push_back(b->run);
    return runs.size();
}

// Fewest runs that chain n beads across a feature.
double chained_runs(size_t n) { return double((std::max<size_t>(n, 1) + 1) / 2); }

struct Crossing { double pos; double width; };

// Where the bead centerlines cross the line {axis = at}, sorted along the other axis.
std::vector<Crossing> cross_section(const std::vector<const Bead *> &beads, int axis, double at)
{
    std::vector<Crossing> out;
    const int other = 1 - axis;
    for (const Bead *b : beads)
        for (size_t i = 0; i + 1 < b->polyline.points.size(); ++ i) {
            const Vec2d p = unscaled(b->polyline.points[i]);
            const Vec2d q = unscaled(b->polyline.points[i + 1]);
            if ((p[axis] <= at && at < q[axis]) || (q[axis] <= at && at < p[axis])) {
                const double f = (at - p[axis]) / (q[axis] - p[axis]);
                out.push_back({ p[other] + f * (q[other] - p[other]), b->width });
            }
        }
    std::sort(out.begin(), out.end(), [](const Crossing &a, const Crossing &b) { return a.pos < b.pos; });
    return out;
}

// Footprint of the beads across a feature of thickness t: from dimension-exact (outer edges on the model
// surface) to volume-exact (spacing = t), and never narrower than the generator's widening floor.
std::pair<double, double> footprint(const Cell &cell, double t)
{
    const double floor = is_arachne(cell) ? min_bead_width + corner : 0.;
    return { std::max(t, floor), std::max(t + corner, floor) };
}

// Allowed width of a single bead across a fin of thickness t.
std::pair<double, double> single_bead_width(const Cell &cell, double t)
{
    const auto [lo, hi] = footprint(cell, t);
    if (is_arachne(cell) && arachne_single_bead == ArachneSingleBead::q1_model_thickness)
        return { lo - bead_tolerance, lo + bead_tolerance };
    return { lo - bead_tolerance, hi + bead_tolerance };
}

// Thinnest feature the cell's generator promises to print.
double presence_threshold(const Cell &cell)
{
    if (is_arachne(cell))
        return min_feature_size;
    if (cell.thin_walls)
        return classic_thin_wall_min;
    // Without thin wall detection classic prints only what holds one outer loop with its outer edge on
    // the surface.
    return wall_width + bead_tolerance;
}

void check_outer_beads(const std::string &feature, const std::vector<Crossing> &xs, double edge_lo, double edge_hi, std::vector<Check> &out)
{
    if (xs.size() < 2)
        return;
    const double w0 = xs.front().width, w1 = xs.back().width;
    out.push_back({ feature, "outer_edge_lo", xs.front().pos - edge_lo, 0.5 * w0 - bead_tolerance, 0.5 * w0 + bead_tolerance, {} });
    out.push_back({ feature, "outer_edge_hi", edge_hi - xs.back().pos, 0.5 * w1 - bead_tolerance, 0.5 * w1 + bead_tolerance, {} });
}

std::string format(const char *f, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

void check_fins(const Cell &cell, const ProbeLayer &layer, std::vector<Check> &out)
{
    const std::vector<Rect> fins = fin_rects();
    for (size_t i = 0; i < fins.size(); ++ i) {
        const double t = fin_thicknesses[i];
        if (t < presence_threshold(cell))
            continue;
        const Vec2d       o = layer.offset;
        const Rect        r { fins[i].x0 + o.x(), fins[i].y0 + o.y(), fins[i].x1 + o.x(), fins[i].y1 + o.y() };
        const std::string feature = format("fin %.2f", t);
        const std::vector<const Bead *> beads = beads_in(layer, around(r));

        // (a)
        out.push_back({ feature, "presence", double(beads.size()), 1., unbounded, "extrusion paths" });
        if (beads.empty())
            continue;

        const std::vector<Crossing> xs = cross_section(beads, 1, 0.5 * (r.y0 + r.y1));
        const auto [w_lo, w_hi] = footprint(cell, t);

        // (b) Cross-section per mm of fin as an equivalent rectangle width (area / layer height), relative to t.
        const double y0 = r.y0 + fin_window_margin, y1 = r.y1 - fin_window_margin;
        const Polygon window = rect_polygon({ r.x0 - attribution_margin, y0, r.x1 + attribution_margin, y1 });
        double volume = 0.;
        for (const Bead *b : beads)
            for (const Polyline &pl : intersection_pl(Polylines{ b->polyline }, window))
                volume += unscaled(pl.length()) * b->mm3_per_mm;
        const double slack = bead_tolerance * double(std::max<size_t>(xs.size(), 1));
        out.push_back({ feature, "volume_ratio", volume / (y1 - y0) / layer_height / t, (w_lo - corner - slack) / t, (w_hi - corner + slack) / t,
                        format("beads across %.0f", double(xs.size())) });

        // (c)
        if (xs.size() == 1) {
            const auto [b_lo, b_hi] = single_bead_width(cell, t);
            out.push_back({ feature, "bead_width", xs.front().width, b_lo, b_hi, {} });
            out.push_back({ feature, "bead_center", std::abs(xs.front().pos - 0.5 * (r.x0 + r.x1)), 0., bead_tolerance, {} });
        }
        // (d)
        check_outer_beads(feature, xs, r.x0, r.x1, out);
        // (f)
        out.push_back({ feature, "runs", double(wall_runs(beads)), 1., chained_runs(xs.size()), {} });
    }
}

void check_block(const ProbeLayer &layer, std::vector<Check> &out)
{
    const Vec2d o = layer.offset;
    const Rect  r { block_x0 + o.x(), block_y0 + o.y(), block_x0 + block_size + o.x(), block_y0 + block_size + o.y() };
    const std::vector<const Bead *> walls = beads_in(layer, around(r), true);
    const std::vector<Crossing>     xs    = cross_section(walls, 1, 0.5 * (r.y0 + r.y1));
    out.push_back({ "block", "wall_beads", double(xs.size()), 2. * wall_loops, 2. * wall_loops, {} });
    check_outer_beads("block", xs, r.x0, r.x1, out);
    out.push_back({ "block", "runs", double(wall_runs(walls)), double(wall_loops), double(wall_loops), {} });
}

void check_wedge(const Cell &cell, const ProbeLayer &layer, std::vector<Check> &out)
{
    const Vec2d o = layer.offset;
    const Rect  r { wedge_x0 + o.x(), wedge_y - 0.5 * wedge_thick + o.y(), wedge_x0 + wedge_length + o.x(), wedge_y + 0.5 * wedge_thick + o.y() };
    const std::vector<const Bead *> beads = beads_in(layer, around(r));

    // (a) The thinnest point of the wedge that still gets extruded.
    double x_min = unbounded;
    for (const Bead *b : beads)
        for (const Point &p : b->polyline.points)
            x_min = std::min(x_min, unscaled(p.x()) - o.x());
    const double cutoff = beads.empty() ? wedge_thick : wedge_thickness_at(std::max(x_min, wedge_x0));
    out.push_back({ "wedge", "cutoff", cutoff, 0., presence_threshold(cell) + wedge_cutoff_allowance, "model thickness at the thin end of the walls" });

    // (f) Against the most beads found across the wedge anywhere along it.
    std::vector<const Bead *> walls;
    for (const Bead *b : beads)
        if (b->wall)
            walls.push_back(b);
    size_t n = 1;
    for (double x = r.x0 + 0.5; x < r.x1; x += 0.5)
        n = std::max(n, cross_section(walls, 0, x).size());
    out.push_back({ "wedge", "runs", double(wall_runs(walls)), 1., chained_runs(n), format("at most %.0f beads across", double(n)) });
}

// (e) The model hole is a 64-gon inscribed in its circle, so its clear diameter is twice the apothem; the
// slices may be simplified by `resolution` on each side. The check carries the worst hole; the detail
// lists the deficit of each.
void check_holes(const ProbeLayer &layer, std::vector<Check> &out)
{
    const Vec2d o = layer.offset;
    const Rect  r { o.x(), plate_y0 + o.y(), hole_cell * double(hole_diameters.size()) + o.x(), plate_y0 + hole_cell + o.y() };
    const std::vector<const Bead *> beads = beads_in(layer, around(r));
    const double apothem = std::cos(PI / hole_segments);
    double      worst = 0.;
    std::string detail = "deficit per hole:";
    for (size_t i = 0; i < hole_diameters.size(); ++ i) {
        const Vec2d c = hole_center(i) + o;
        double clear_radius = unbounded;
        for (const Bead *b : beads)
            for (size_t k = 0; k + 1 < b->polyline.points.size(); ++ k) {
                const Vec2d  p = unscaled(b->polyline.points[k]);
                const Vec2d  v = unscaled(b->polyline.points[k + 1]) - p;
                const double f = v.squaredNorm() > 0. ? std::clamp((c - p).dot(v) / v.squaredNorm(), 0., 1.) : 0.;
                clear_radius = std::min(clear_radius, (p + f * v - c).norm() - 0.5 * b->width);
            }
        const double deficit = hole_diameters[i] * apothem - 2. * std::max(clear_radius, 0.);
        detail += format(" %.1f:", hole_diameters[i]) + format("%+.3f", -deficit);
        worst = std::max(worst, std::abs(deficit));
    }
    out.push_back({ "holes", "worst_diameter", worst, 0., 2. * (resolution + bead_tolerance), detail });
}

std::vector<Check> evaluate(const Cell &cell)
{
    const ProbeLayer layer = slice_probe_layer(cell);
    std::vector<Check> out;
    check_fins(cell, layer, out);
    check_block(layer, out);
    check_wedge(cell, layer, out);
    check_holes(layer, out);
    return out;
}

// (c') The pseudo cell comparing the two Arachne cells. Every fin that precise_outer_wall=0 prints as one
// bead must come out as one bead of the same width with precise_outer_wall=1; a missing or split bead
// counts as an unbounded difference.
const char *const precise_parity       = "arachne/precise=1 vs 0";
const char *const precise_shift_parity = "arachne/precise=1/shift vs 0";
// The pseudo cell comparing the two precise methods: a wall with room behind its beads must come out the
// same either way, bead for bead and with the same infill region, and toolpath_shift may not print a wall
// narrower than outline_shrink does anywhere on the plate.
const char *const precise_methods = "arachne/precise=1/shift vs precise=1";

std::vector<Crossing> fin_crossings(const ProbeLayer &layer, size_t i)
{
    const Rect  f = fin_rects()[i];
    const Vec2d o = layer.offset;
    const Rect  r { f.x0 + o.x(), f.y0 + o.y(), f.x1 + o.x(), f.y1 + o.y() };
    return cross_section(beads_in(layer, around(r)), 1, 0.5 * (r.y0 + r.y1));
}

std::vector<Check> evaluate_precise_parity(const char *precise_cell)
{
    const ProbeLayer off = slice_probe_layer(cell_named("arachne/precise=0"));
    const ProbeLayer on  = slice_probe_layer(cell_named(precise_cell));
    std::vector<Check> out;
    for (size_t i = 0; i < fin_thicknesses.size(); ++ i) {
        const std::vector<Crossing> a = fin_crossings(off, i);
        if (a.size() != 1)
            continue;
        const std::vector<Crossing> b = fin_crossings(on, i);
        const double diff = b.size() == 1 ? std::abs(b.front().width - a.front().width) : unbounded;
        out.push_back({ format("fin %.2f", fin_thicknesses[i]), "precise_parity", diff, 0., bead_tolerance,
                        format("off %.4f", a.front().width) + (b.size() == 1 ? format(" on %.4f", b.front().width) :
                                                                               format(" on: %.0f beads", double(b.size()))) });
    }
    return out;
}

std::vector<Crossing> block_crossings(const ProbeLayer &layer)
{
    const Vec2d o = layer.offset;
    const Rect  r { block_x0 + o.x(), block_y0 + o.y(), block_x0 + block_size + o.x(), block_y0 + block_size + o.y() };
    return cross_section(beads_in(layer, around(r), true), 1, 0.5 * (r.y0 + r.y1));
}

// Extents of the block's infill region, model frame.
BoundingBoxf block_fill_extents(const ProbeLayer &layer)
{
    const Vec2d o = layer.offset;
    const Polygon block = rect_polygon({ block_x0 + o.x(), block_y0 + o.y(), block_x0 + block_size + o.x(), block_y0 + block_size + o.y() });
    const BoundingBox box = get_extents(intersection_ex(layer.fill_area, ExPolygons{ ExPolygon(block) }));
    return box.defined ? BoundingBoxf(unscaled(box.min) - o, unscaled(box.max) - o) : BoundingBoxf();
}

double narrowest_wall(const ProbeLayer &layer)
{
    double out = unbounded;
    for (const Bead &b : layer.beads)
        if (b.wall && b.polyline.length() > 0)
            out = std::min(out, b.width);
    return out;
}

std::vector<Check> evaluate_precise_methods()
{
    const ProbeLayer shrink_layer = slice_probe_layer(cell_named("arachne/precise=1"));
    const ProbeLayer shift_layer  = slice_probe_layer(cell_named("arachne/precise=1/shift"));
    const std::vector<Crossing> shrink = block_crossings(shrink_layer);
    const std::vector<Crossing> shift  = block_crossings(shift_layer);
    std::vector<Check> out;
    // The infill region follows the walls: it may not move with the method either (wall analysis 3.3).
    const BoundingBoxf fill_shrink = block_fill_extents(shrink_layer);
    const BoundingBoxf fill_shift  = block_fill_extents(shift_layer);
    if (fill_shrink.defined && fill_shift.defined) {
        const double moved = std::max((fill_shift.min - fill_shrink.min).cwiseAbs().maxCoeff(), (fill_shift.max - fill_shrink.max).cwiseAbs().maxCoeff());
        out.push_back({ "block", "fill_extents", moved, 0., bead_tolerance,
                        format("shrink x0 %.4f", fill_shrink.min.x()) + format(" shift x0 %.4f", fill_shift.min.x()) });
    } else
        out.push_back({ "block", "fill_extents", unbounded, 0., bead_tolerance, "no infill region in the block" });
    // Criterion Q7: no wall anywhere on the plate comes out narrower than the narrowest wall of the
    // outline-shrink method.
    const double narrowest_shrink = narrowest_wall(shrink_layer);
    out.push_back({ "plate", "narrowest_wall", narrowest_wall(shift_layer), narrowest_shrink - bead_tolerance, unbounded,
                    format("shrink %.4f", narrowest_shrink) });
    out.push_back({ "block", "wall_beads", double(shift.size()), double(shrink.size()), double(shrink.size()), {} });
    for (size_t i = 0; i < std::min(shrink.size(), shift.size()); ++ i) {
        out.push_back({ format("bead %.0f", double(i)), "position", std::abs(shift[i].pos - shrink[i].pos), 0., bead_tolerance,
                        format("shrink %.4f", shrink[i].pos) + format(" shift %.4f", shift[i].pos) });
        out.push_back({ format("bead %.0f", double(i)), "width", std::abs(shift[i].width - shrink[i].width), 0., bead_tolerance,
                        format("shrink %.4f", shrink[i].width) + format(" shift %.4f", shift[i].width) });
    }
    return out;
}

std::vector<Check> evaluate_named(const std::string &name)
{
    if (name == precise_parity)
        return evaluate_precise_parity("arachne/precise=1");
    if (name == precise_shift_parity)
        return evaluate_precise_parity("arachne/precise=1/shift");
    if (name == precise_methods)
        return evaluate_precise_methods();
    return evaluate(cell_named(name));
}

// ---- Known defects ----

struct KnownFailure {
    const char *defect;
    const char *cell;
    const char *feature;
    const char *kind;
};

// Checks that disagree with the bead model on the current code, by defect (IDs of the wall analysis):
//   WALL-1  Arachne with precise_outer_wall shrinks the outline before beading
//           (PerimeterGenerator::process_arachne), so features below min_feature_size + 0.086 mm vanish,
//           single beads come out 0.043 mm narrow and multi-bead walls lose their overlap.
//   WALL-3  Classic walls narrower than their loop count: collapsed loops overfill 0.45-0.7 mm walls and
//           loops spaced at full width underfill 0.8 mm walls.
//   CLASSIC-PRECISE  Classic with precise_outer_wall spaces the first inner wall at full width
//           (ext_perimeter_spacing2), so 0.9-1.2 mm walls lose their overlap; same symptom as WALL-1.
//   WALL-4  Single-bead tails are not chained to their parent loop.
//   WALL-5  Arachne toolpath simplification makes holes undersized.
const std::vector<KnownFailure> &known_failures()
{
    static const std::vector<KnownFailure> list = {
        { "WALL-1 presence", "arachne/precise=1", "fin 0.15", "presence" },
        { "WALL-1 presence", "arachne/precise=1", "wedge", "cutoff" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.40", "bead_width" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.45", "bead_width" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.50", "bead_width" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.60", "bead_width" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.40", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.45", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.50", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.60", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.70", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.80", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 0.90", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 1.00", "volume_ratio" },
        { "WALL-1 width", "arachne/precise=1", "fin 1.20", "volume_ratio" },
        { "WALL-1 presence", precise_parity, "fin 0.15", "precise_parity" },
        { "WALL-1 width", precise_parity, "fin 0.40", "precise_parity" },
        { "WALL-1 width", precise_parity, "fin 0.45", "precise_parity" },
        { "WALL-1 width", precise_parity, "fin 0.50", "precise_parity" },
        { "WALL-1 width", precise_parity, "fin 0.60", "precise_parity" },

        { "WALL-3", "classic/precise=0", "fin 0.45", "volume_ratio" },
        { "WALL-3", "classic/precise=0", "fin 0.50", "volume_ratio" },
        { "WALL-3", "classic/precise=0", "fin 0.60", "volume_ratio" },
        { "WALL-3", "classic/precise=0", "fin 0.70", "volume_ratio" },
        { "WALL-3", "classic/precise=0", "fin 0.80", "volume_ratio" },
        { "WALL-3", "classic/precise=1", "fin 0.45", "volume_ratio" },
        { "WALL-3", "classic/precise=1", "fin 0.50", "volume_ratio" },
        { "WALL-3", "classic/precise=1", "fin 0.60", "volume_ratio" },
        { "WALL-3", "classic/precise=1", "fin 0.70", "volume_ratio" },
        { "WALL-3", "classic/precise=1", "fin 0.80", "volume_ratio" },
        { "WALL-3", "classic+thin/precise=0", "fin 0.70", "volume_ratio" },
        { "WALL-3", "classic+thin/precise=0", "fin 0.80", "volume_ratio" },
        { "WALL-3", "classic+thin/precise=1", "fin 0.70", "volume_ratio" },
        { "WALL-3", "classic+thin/precise=1", "fin 0.80", "volume_ratio" },

        { "CLASSIC-PRECISE", "classic/precise=1", "fin 0.90", "volume_ratio" },
        { "CLASSIC-PRECISE", "classic/precise=1", "fin 1.00", "volume_ratio" },
        { "CLASSIC-PRECISE", "classic/precise=1", "fin 1.20", "volume_ratio" },
        { "CLASSIC-PRECISE", "classic+thin/precise=1", "fin 0.90", "volume_ratio" },
        { "CLASSIC-PRECISE", "classic+thin/precise=1", "fin 1.00", "volume_ratio" },
        { "CLASSIC-PRECISE", "classic+thin/precise=1", "fin 1.20", "volume_ratio" },

        { "WALL-4", "arachne/precise=0", "wedge", "runs" },
        { "WALL-4", "arachne/precise=1", "wedge", "runs" },
        { "WALL-4", "arachne/precise=1/shift", "wedge", "runs" },
        { "WALL-4", "classic+thin/precise=0", "wedge", "runs" },
        { "WALL-4", "classic+thin/precise=1", "wedge", "runs" },

        { "WALL-5", "arachne/precise=0", "holes", "worst_diameter" },
        { "WALL-5", "arachne/precise=1", "holes", "worst_diameter" },
        { "WALL-5", "arachne/precise=1/shift", "holes", "worst_diameter" },
    };
    return list;
}

const KnownFailure *known_failure(const std::string &cell_name, const Check &check)
{
    for (const KnownFailure &k : known_failures())
        if (check.feature == k.feature && check.kind == k.kind && cell_name == k.cell)
            return &k;
    return nullptr;
}

// Every check of the cell agrees with the bead model, except the known defects, which must still disagree.
void require_bead_model(const std::string &cell_name)
{
    const std::vector<Check> checks = evaluate_named(cell_name);
    for (const KnownFailure &k : known_failures())
        if (cell_name == k.cell) {
            INFO("known failure " << k.feature << " " << k.kind << " (" << k.defect << ") matches no check");
            CHECK(std::any_of(checks.begin(), checks.end(), [&k](const Check &c) { return c.feature == k.feature && c.kind == k.kind; }));
        }
    for (const Check &c : checks) {
        INFO(cell_name << ": " << c.describe());
        if (const KnownFailure *k = known_failure(cell_name, c)) {
            INFO("listed as a known " << k->defect << " failure but agrees with the bead model now: "
                 "remove it from known_failures() and check the defect's [!shouldfail] test case");
            CHECK_FALSE(c.pass());
        } else
            CHECK(c.pass());
    }
}

// The checks listed for `defect`, asserted as normal checks.
void require_defect_fixed(const char *defect)
{
    std::vector<std::string> cell_names;
    for (const KnownFailure &k : known_failures())
        if (std::string(k.defect) == defect && std::find(cell_names.begin(), cell_names.end(), k.cell) == cell_names.end())
            cell_names.emplace_back(k.cell);
    REQUIRE_FALSE(cell_names.empty());
    for (const std::string &name : cell_names) {
        for (const Check &c : evaluate_named(name))
            if (const KnownFailure *k = known_failure(name, c); k != nullptr && std::string(k->defect) == defect) {
                INFO(name << ": " << c.describe());
                CHECK(c.pass());
            }
    }
}

} // namespace

TEST_CASE("Arachne walls without precise outer wall match the bead model", "[WallGeometry]")
{
    require_bead_model("arachne/precise=0");
}

TEST_CASE("Arachne walls with precise outer wall match the bead model", "[WallGeometry]")
{
    require_bead_model("arachne/precise=1");
}

// Criterion Q1: precise_outer_wall must not change the width of an Arachne single bead.
TEST_CASE("Arachne single beads are as wide with precise outer wall as without", "[WallGeometry]")
{
    require_bead_model(precise_parity);
}

// The toolpath-shift method is the fix for WALL-1: with it the precise outer wall passes every check
// that the outline-shrink method is listed for in known_failures().
TEST_CASE("Arachne walls with precise outer wall by toolpath shift match the bead model", "[WallGeometry]")
{
    require_bead_model("arachne/precise=1/shift");
}

TEST_CASE("Arachne single beads are as wide with precise outer wall by toolpath shift as without", "[WallGeometry]")
{
    require_bead_model(precise_shift_parity);
}

TEST_CASE("Both precise outer wall methods place the walls and the infill of a thick wall alike", "[WallGeometry]")
{
    require_bead_model(precise_methods);
}

// ---- A covered rib next to a top surface (only_one_wall_top) ----
//
// With only_one_wall_top the layer under a top surface gets its outer wall and the walls behind it from two
// separate Arachne runs (PerimeterGenerator::process_arachne), not from one run over the whole wall as on a
// plain layer. A rib that continues upwards from such a layer must get its walls at the same places as on a
// plain layer, and the precise outer wall may not make them narrower than they are without it on that same
// layer (criterion Q1, applied to this second code path).

namespace {

// Rib thicknesses: 1.2 mm leaves one bead for the run behind the outer wall, which never moves; 2.0 mm
// leaves three, whose outer two have to move with the toolpath shift as the walls behind the outer wall
// do on a plain layer.
const std::vector<double> rib_thicknesses = { 1.2, 2.0 };
constexpr double rib_length      = 12.;
constexpr double rib_height      = 2.0;
constexpr double terrace_half    = 3.;   // the terrace ends at z = terrace_top and leaves the rib standing
constexpr double terrace_top     = 1.0;
constexpr double terrace_y0      = 3., terrace_y1 = 9.;
constexpr double rib_probe_y     = 10.5; // across the rib, clear of the terrace and of the rib's end
constexpr double rib_plain_z     = 0.6;  // a layer with no top surface
constexpr double rib_terrace_z   = terrace_top; // the layer whose top surface is the terrace

struct RibWalls {
    std::vector<Crossing> plain;   // across the rib on the plain layer, model frame
    std::vector<Crossing> terrace; // across the rib on the terrace layer, model frame
};

std::vector<Crossing> rib_cross_section(const Layer *layer)
{
    ProbeLayer out;
    out.offset = unscaled(get_extents(layer->lslices).min);
    size_t run = 0;
    for (const LayerRegion *region : layer->regions())
        collect(&region->perimeters, true, run, out.beads);
    std::vector<const Bead *> walls;
    for (const Bead &b : out.beads)
        walls.push_back(&b);
    std::vector<Crossing> xs = cross_section(walls, 1, rib_probe_y + out.offset.y());
    for (Crossing &c : xs)
        c.pos -= out.offset.x();
    return xs;
}

RibWalls rib_walls(const Cell &cell, double rib_thickness)
{
    TriangleMesh rib     = extrude({ ExPolygon(rect_polygon({ 0., 0., rib_thickness, rib_length })) }, rib_height);
    TriangleMesh terrace = extrude({ ExPolygon(rect_polygon({ 0.5 * rib_thickness - terrace_half, terrace_y0, 0.5 * rib_thickness + terrace_half, terrace_y1 })) }, terrace_top);
    indexed_triangle_set its = rib.its;
    its_merge(its, terrace.its);

    DynamicPrintConfig config = cell_config(cell);
    config.set_deserialize_strict({ { "only_one_wall_top", true } });

    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(TriangleMesh(std::move(its)));
    init_print(std::move(meshes), print, model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);

    const Layer *plain = nullptr, *top = nullptr;
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - rib_plain_z) < EPSILON)
            plain = layer;
        if (std::abs(layer->print_z - rib_terrace_z) < EPSILON)
            top = layer;
    }
    REQUIRE(plain != nullptr);
    REQUIRE(top != nullptr);
    // The terrace layer holds a top surface: the terrace is not covered by the layer above.
    REQUIRE(top->upper_layer != nullptr);
    REQUIRE(area(diff(to_polygons(top->lslices), to_polygons(top->upper_layer->lslices))) > 0);

    return { rib_cross_section(plain), rib_cross_section(top) };
}

// The rib's walls sit at the same places on both layers, and on the terrace layer they are as wide as
// without the precise outer wall.
void require_rib_walls_alike(const char *cell_name)
{
    for (double t : rib_thicknesses) {
        DYNAMIC_SECTION("rib " << t << " mm") {
            const RibWalls              walls     = rib_walls(cell_named(cell_name), t);
            const std::vector<Crossing> reference = rib_walls(cell_named("arachne/precise=0"), t).terrace;
            INFO(cell_name << ": " << walls.plain.size() << " beads across the rib on the plain layer, " << walls.terrace.size() << " on the terrace layer");
            REQUIRE(walls.plain.size() >= size_t(std::round(t / wall_width)));
            REQUIRE(walls.terrace.size() == walls.plain.size());
            REQUIRE(reference.size() == walls.plain.size());
            for (size_t i = 0; i < walls.plain.size(); ++ i) {
                INFO(cell_name << ": bead " << i << " at " << walls.plain[i].pos << " on the plain layer, at " << walls.terrace[i].pos
                     << " width " << walls.terrace[i].width << " on the terrace layer, " << reference[i].width << " wide there without the precise outer wall");
                CHECK_THAT(walls.terrace[i].pos, Catch::Matchers::WithinAbs(walls.plain[i].pos, bead_tolerance));
                CHECK_THAT(walls.terrace[i].width, Catch::Matchers::WithinAbs(reference[i].width, bead_tolerance));
            }
        }
    }
}

} // namespace

TEST_CASE("Arachne walls of a rib beside a top surface sit where they sit on a plain layer", "[WallGeometry]")
{
    require_rib_walls_alike("arachne/precise=0");
}

TEST_CASE("Arachne precise outer wall by toolpath shift keeps the walls of a rib beside a top surface", "[WallGeometry]")
{
    require_rib_walls_alike("arachne/precise=1/shift");
}

// WALL-1 on the only_one_wall_top path: the outline-shrink method takes the precise wall gap out of the
// region of the walls behind the outer wall, so the rib's middle bead comes out narrower than without the
// precise outer wall.
TEST_CASE("Arachne precise outer wall by outline shrink keeps the walls of a rib beside a top surface", "[WallGeometry][!shouldfail]")
{
    require_rib_walls_alike("arachne/precise=1");
}

TEST_CASE("Classic walls without precise outer wall match the bead model", "[WallGeometry]")
{
    require_bead_model("classic/precise=0");
}

TEST_CASE("Classic walls with precise outer wall match the bead model", "[WallGeometry]")
{
    require_bead_model("classic/precise=1");
}

TEST_CASE("Classic thin walls without precise outer wall match the bead model", "[WallGeometry]")
{
    require_bead_model("classic+thin/precise=0");
}

TEST_CASE("Classic thin walls with precise outer wall match the bead model", "[WallGeometry]")
{
    require_bead_model("classic+thin/precise=1");
}

// Known defects. Each test case below fails while its defect exists, which [!shouldfail] turns into a
// pass. When a fix makes one pass, drop the tag and the defect's known_failures() entries.

// WALL-1: precise outer wall drops the 0.15 mm fin and cuts the wedge off at 0.187 mm.
TEST_CASE("Arachne with precise outer wall prints every feature down to min_feature_size", "[WallGeometry][!shouldfail]")
{
    require_defect_fixed("WALL-1 presence");
}

// WALL-1: precise outer wall prints the 0.45 mm fin 0.407 mm wide and underfills multi-bead walls.
TEST_CASE("Arachne with precise outer wall prints beads as wide as the feature", "[WallGeometry][!shouldfail]")
{
    require_defect_fixed("WALL-1 width");
}

TEST_CASE("Classic walls narrower than their loop count are neither collapsed nor unoverlapped", "[WallGeometry][!shouldfail]")
{
    require_defect_fixed("WALL-3");
}

TEST_CASE("Classic precise outer wall keeps the overlap of the first inner wall", "[WallGeometry][!shouldfail]")
{
    require_defect_fixed("CLASSIC-PRECISE");
}

TEST_CASE("Single-bead tails are chained to their parent loop", "[WallGeometry][!shouldfail]")
{
    require_defect_fixed("WALL-4");
}

TEST_CASE("Arachne keeps hole diameters within the slice resolution", "[WallGeometry][!shouldfail]")
{
    require_defect_fixed("WALL-5");
}

// Prints every measurement of every cell, for comparing two builds. Hidden: it asserts nothing.
TEST_CASE("Wall geometry report", "[.][WallGeometryReport]")
{
    std::vector<std::string> names;
    for (const Cell &cell : cells)
        names.emplace_back(cell.name);
    names.emplace_back(precise_parity);
    names.emplace_back(precise_shift_parity);
    names.emplace_back(precise_methods);
    for (const std::string &name : names) {
        std::string s = "== " + name + "\n";
        for (const Check &c : evaluate_named(name)) {
            s += "  " + c.describe();
            if (const KnownFailure *k = known_failure(name, c))
                s += std::string("  [known ") + k->defect + "]";
            s += "\n";
        }
        WARN(s);
    }
}
