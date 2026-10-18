#include <catch2/catch_all.hpp>

#include <algorithm>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

// The upper plate overhangs both the lower plate and open air, so branches land on the model and on
// the bed in the same slice.
TriangleMesh two_tier_mesh()
{
    TriangleMesh lower  = make_cube(30, 30, 3);
    TriangleMesh column = make_cube(8, 8, 15);
    TriangleMesh upper  = make_cube(50, 50, 3);
    // Each part overlaps the one below rather than resting on it; a coplanar join slices ambiguously.
    column.translate(11.f, 11.f, 2.f);
    upper.translate(-10.f, -10.f, 16.f);
    TriangleMesh mesh = lower;
    mesh.merge(column);
    mesh.merge(upper);
    return mesh;
}

TriangleMesh scaled(TestMesh id, float scale)
{
    TriangleMesh mesh = Slic3r::Test::mesh(id);
    mesh.scale(scale);
    return mesh;
}

// `extra` is applied last, so a caller can add or override any key.
void slice_with_tree_support(const TriangleMesh &mesh, Slic3r::Print &print, const char *style,
                             int threshold_angle = 30, int build_plate_only = 0, int raft_layers = 0,
                             std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                "tree(auto)" },
        { "support_style",               style },
        { "support_on_build_plate_only", build_plate_only },
        { "support_threshold_angle",     threshold_angle },
        { "raft_layers",                 raft_layers },
        { "layer_height",                0.2 },
    });
    config.set_deserialize_strict(extra);
    Slic3r::Test::init_and_process_print({ mesh }, print, config);
}

Points support_points(const Slic3r::Print &print)
{
    Points points;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        layer->support_fills.collect_points(points);
    return points;
}

size_t support_point_count(const TriangleMesh &mesh, const char *style, int threshold_angle = 30,
                           int build_plate_only = 0)
{
    Slic3r::Print print;
    slice_with_tree_support(mesh, print, style, threshold_angle, build_plate_only);
    return support_points(print).size();
}

// Index of the first differing point, or the common length when they match. An index keeps a
// failure readable; comparing the vectors themselves dumps thousands of points.
size_t first_difference(const Points &a, const Points &b)
{
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i)
        if (a[i] != b[i])
            return i;
    return common;
}

// Slice `mesh` twice and require an identical support point sequence. Point counts and total
// length are order insensitive, so the sequence is what a reordering shows up in.
void sliced_twice_matches(const TriangleMesh &mesh, int build_plate_only, const char *style = "tree_slim",
                          std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    Slic3r::Print first_print, second_print;
    slice_with_tree_support(mesh, first_print, style, 30, build_plate_only, 0, extra);
    slice_with_tree_support(mesh, second_print, style, 30, build_plate_only, 0, extra);
    const Points first  = support_points(first_print);
    const Points second = support_points(second_print);
    REQUIRE(first.size() > 1000); // without support the comparison below passes vacuously
    REQUIRE(second.size() == first.size());
    REQUIRE(first_difference(first, second) == first.size());
}

} // namespace

TEST_CASE("Tree support is generated for an overhang and not for a plain cube", "[TreeSupport]")
{
    REQUIRE(support_point_count(scaled(TestMesh::overhang, 2.f), "tree_slim") > 1000);
    REQUIRE(support_point_count(Slic3r::Test::cube(20), "tree_slim") == 0);
}

TEST_CASE("Restricting tree support to the build plate changes what is generated", "[TreeSupport]")
{
    const TriangleMesh mesh = two_tier_mesh();
    const size_t anywhere    = support_point_count(mesh, "tree_slim", 30, 0);
    const size_t plate_only  = support_point_count(mesh, "tree_slim", 30, 1);
    REQUIRE(anywhere > 1000);
    REQUIRE(plate_only > 1000);
    // The upper plate overhangs the lower one, so some branches would land on the model.
    REQUIRE(plate_only != anywhere);
}

TEST_CASE("Tree support layers rise monotonically within the layer height limits", "[TreeSupport]")
{
    Slic3r::Print print;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), print, "tree_slim");
    const double nozzle = print.config().nozzle_diameter.values.front();

    size_t checked = 0;
    double previous = 0;
    bool   previous_was_adjacent = false;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        if (layer->print_z <= 0 || layer->height <= 0) {
            // Layers with no nodes are left at zero. Skipping one leaves a hole, so the next pair
            // spans more than one layer and its gap says nothing about the layer height limit.
            previous_was_adjacent = false;
            continue;
        }
        if (previous > 0) {
            CAPTURE(previous, layer->print_z);
            REQUIRE(layer->print_z > previous);
            if (previous_was_adjacent)
                REQUIRE(layer->print_z - previous <= nozzle + EPSILON);
        }
        previous = layer->print_z;
        previous_was_adjacent = true;
        ++checked;
    }
    REQUIRE(checked > 10);
}

TEST_CASE("A raft is still generated under tree support", "[TreeSupport]")
{
    // The mesh supports itself, so a layer count alone passes with no raft at all.
    Slic3r::Print rafted, unrafted;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), rafted, "tree_slim", 30, 0, 3);
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), unrafted, "tree_slim", 30, 0, 0);
    const PrintObject *rafted_object   = rafted.objects().front();
    const PrintObject *unrafted_object = unrafted.objects().front();
    REQUIRE(rafted_object->support_layers().size() > unrafted_object->support_layers().size());
    // The raft goes under the object.
    REQUIRE(rafted_object->layers().front()->print_z > unrafted_object->layers().front()->print_z);
}

// drop_nodes() decides the node merges and spawns the next layer's nodes in parallel. Every one of
// those decisions has to be applied in a fixed order, or the same model gives different branches on
// each slice.
TEST_CASE("Tree support toolpaths do not depend on thread scheduling", "[TreeSupport][Regression]")
{
    // Scaled up so that a layer holds enough nodes for the parallel range to be split. At stock
    // size it stays in one chunk and the order never varies.
    SECTION("overhang")            { sliced_twice_matches(scaled(TestMesh::overhang, 2.f), 0); }
    SECTION("bridge with hole")    { sliced_twice_matches(scaled(TestMesh::bridge_with_hole, 3.f), 0); }
    // Dropping every branch that cannot reach the bed leaves the survivors dense enough that the
    // neighbour merge fires in bulk.
    SECTION("on the build plate")  { sliced_twice_matches(scaled(TestMesh::overhang, 4.f), 1); }
    // Branches resting on the model are what put nodes in a part group other than 0, which is the
    // only way to reach the prune in the second pass. tree_hybrid additionally builds polygon
    // nodes, so it is the only style that exercises the overhang merge.
    SECTION("resting on the model") { sliced_twice_matches(two_tier_mesh(), 0); }
    SECTION("hybrid on the model")  { sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid"); }
}

// Prim breaks equal-distance ties by heap address. A 1 mm branch diameter puts neighbours close
// enough to tie, and an explicit line width pins max_move_dist, so the moved tie winner reaches
// the support toolpaths.
TEST_CASE("Tree support toolpaths do not depend on the MST tie order", "[TreeSupport][Regression]")
{
    sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid", {
        { "tree_support_branch_diameter", 1.0 },
        { "tree_support_branch_distance", 5.0 },
        { "tree_support_branch_angle",    40 },
        { "support_line_width",           0.4 },
    });
}

namespace {

// An explicit line width: the sharp-tail detection erodes by multiples of it, and the built-in
// default resolves to zero here, which would leave the detection inert.
constexpr double tree_line_width = 0.42;

// A fin standing on the bed, 5 mm thick at the foot and 20 mm long, whose right face leans outwards
// at 80 degrees to the bed (10 degrees from vertical): 0.88 mm of overhang over 5 mm of height, far
// inside a 30 degree support threshold.
TriangleMesh bed_standing_fin()
{
    return TriangleMesh(
        {
            {0.f, 0.f, 0.f},   {5.f, 0.f, 0.f},   {5.f, 20.f, 0.f},   {0.f, 20.f, 0.f},
            {0.f, 0.f, 5.f},   {5.88f, 0.f, 5.f}, {5.88f, 20.f, 5.f}, {0.f, 20.f, 5.f},
        },
        {
            {0, 2, 1}, {0, 3, 2}, // bottom
            {4, 5, 6}, {4, 6, 7}, // top
            {0, 1, 5}, {0, 5, 4}, // front
            {2, 3, 7}, {2, 7, 6}, // back
            {0, 4, 7}, {0, 7, 3}, // left
            {1, 2, 6}, {1, 6, 5}, // right, the leaning face
        });
}

// A 10 x 24 x 2 mm block on the bed and, beside it, a 4 x 12 mm tail floating 6 mm above the bed.
// The tail is narrow enough to be classified as a sharp tail all the way up, and leans 10 degrees
// from vertical over its 10 mm. It hangs over the bed rather than over the block, so the branches
// under it have room to land.
TriangleMesh block_with_leaning_floating_tail()
{
    TriangleMesh mesh = make_cube(10., 24., 2.);
    TriangleMesh tail(
        {
            {20.f, 2.f, 6.f},     {24.f, 2.f, 6.f},     {24.f, 14.f, 6.f},     {20.f, 14.f, 6.f},
            {21.76f, 2.f, 16.f},  {25.76f, 2.f, 16.f},  {25.76f, 14.f, 16.f},  {21.76f, 14.f, 16.f},
        },
        {
            {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
            {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
        });
    mesh.merge(tail);
    return mesh;
}

// Highest print_z of a support layer that carries any support extrusion.
double highest_supported_z(const Slic3r::Print &print)
{
    double z = 0.;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        if (!layer->support_fills.empty())
            z = std::max(z, layer->print_z);
    return z;
}

} // namespace

TEST_CASE("A narrow fin standing on the bed gets no tree support under its near-vertical face", "[TreeSupport][Regression]")
{
    // The bed layer used to seed the sharp-tail classification from any footprint under 6 mm, and
    // every layer above it then got support against the merely resolution-expanded layer below, so
    // an 80 degree face was supported as if it were a tail hanging in the air.
    const char *style = GENERATE("tree_slim", "tree_strong", "tree_hybrid");
    INFO("style = " << style);
    Slic3r::Print print;
    slice_with_tree_support(bed_standing_fin(), print, style, 30, 0, 0, {{ "line_width", tree_line_width }});
    CHECK(support_points(print).empty());
}

TEST_CASE("A tail hanging in the air is supported only where it overhangs beyond the threshold angle", "[TreeSupport][Regression]")
{
    // The tail's underside at z = 6 hangs in the air and gets support. Its faces lean 10 degrees
    // from vertical, inside the 30 degree threshold, so nothing above the underside is an overhang;
    // the tail support used to climb the leaning face to the top of the tail regardless.
    Slic3r::Print print;
    slice_with_tree_support(block_with_leaning_floating_tail(), print, "tree_slim", 30, 0, 0, {{ "line_width", tree_line_width }});
    REQUIRE_FALSE(support_points(print).empty());
    CHECK(highest_supported_z(print) < 6.5);
}

TEST_CASE("The underside of a tail hanging beside a wall is an overhang whatever the threshold angle", "[TreeSupport][Regression]")
{
    // A 0.8 mm wide bar starts in the air at z = 6, 0.2 mm away from a block. At a 10 degree
    // threshold the block's slices grown by the threshold offset (0.2 / tan(11 deg), about 1 mm)
    // cover the whole bar, so neither the ordinary overhang detection nor a threshold-grown tail
    // subtraction sees its underside; the bar's first layer still hangs in the air.
    TriangleMesh mesh = make_cube(10., 24., 16.);
    TriangleMesh bar  = make_cube(0.8, 12., 10.);
    bar.translate(10.2f, 6.f, 6.f);
    mesh.merge(bar);
    Slic3r::Print print;
    slice_with_tree_support(mesh, print, "tree_slim", 10, 0, 0, {{ "line_width", tree_line_width }});
    size_t overhang_layers = 0;
    for (const Layer *layer : print.objects().front()->layers())
        overhang_layers += !layer->loverhangs.empty();
    CHECK(overhang_layers == 1);
}

TEST_CASE("Tree support under sharp tails does not depend on thread scheduling", "[TreeSupport][Regression]")
{
    // The overhang detection reduced its per-layer findings through shared flags written from the
    // parallel loop, one of them gated on wall-clock time.
    sliced_twice_matches(block_with_leaning_floating_tail(), 0, "tree_slim", {{ "line_width", tree_line_width }});
    sliced_twice_matches(block_with_leaning_floating_tail(), 0, "tree_hybrid", {{ "line_width", tree_line_width }});
}

namespace {

// An 8 x 8 mm column carrying a 30 x 30 mm plate at z = 12. The support under the plate stands
// around the column, so its clearance to the column is the XY distance the generator applied.
TriangleMesh column_under_plate()
{
    TriangleMesh column = make_cube(8., 8., 13.);
    TriangleMesh plate  = make_cube(30., 30., 3.);
    column.translate(11.f, 11.f, 0.f);
    plate.translate(0.f, 0.f, 12.f);
    column.merge(plate);
    return column;
}

void collect_paths(const ExtrusionEntity &entity, std::vector<const ExtrusionPath *> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_paths(*child, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        out.push_back(path);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &p : multi->paths)
            out.push_back(&p);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &p : loop->paths)
            out.push_back(&p);
    }
}

// Smallest gap, in mm, between the edge of any support bead and the model slice printed at the
// same height. Layer 0 is skipped: it takes support_object_first_layer_gap instead.
double smallest_support_clearance(const Slic3r::Print &print)
{
    const PrintObject *object = print.objects().front();
    double smallest = std::numeric_limits<double>::max();
    for (const SupportLayer *support_layer : object->support_layers()) {
        const Layer *model_layer = nullptr;
        for (const Layer *layer : object->layers())
            if (std::abs(layer->print_z - support_layer->print_z) < EPSILON)
                model_layer = layer;
        if (model_layer == nullptr || model_layer->id() == 0 || model_layer->lslices.empty())
            continue;
        std::vector<const ExtrusionPath *> paths;
        collect_paths(support_layer->support_fills, paths);
        const Polygons model = to_polygons(model_layer->lslices);
        for (const ExtrusionPath *path : paths) {
            const Polyline line = path->polyline.to_polyline();
            if (line.size() < 2)
                continue;
            // Sample the bead centre line finely enough that a segment passing a corner is seen.
            Points samples = line.equally_spaced_points(Slic3r::scaled<double>(0.02));
            samples.push_back(line.last_point());
            for (const Point &pt : samples) {
                double centre = std::numeric_limits<double>::max();
                for (const Polygon &polygon : model) {
                    if (polygon.is_counter_clockwise() && polygon.contains(pt))
                        centre = 0.;
                    for (const Line &edge : polygon.lines())
                        centre = std::min(centre, unscaled<double>(edge.distance_to(pt)));
                }
                smallest = std::min(smallest, centre - 0.5 * path->width);
            }
        }
    }
    return smallest;
}

} // namespace

TEST_CASE("Organic tree support keeps the configured XY distance from the model", "[TreeSupport][Regression]")
{
    // The organic generator kept every branch near an overhang half the outer wall width (0.21 mm here)
    // from the model, whatever support_object_xy_distance said: 0.8 mm configured gave the same
    // 0.43 mm bead clearance as 0.35 mm. 0.35 mm guards the small end, which was already met.
    const double xy_distance = GENERATE(0.35, 0.8);
    INFO("support_object_xy_distance = " << xy_distance);
    Slic3r::Print print;
    slice_with_tree_support(column_under_plate(), print, "organic", 30, 0, 0, {
        { "line_width",                 tree_line_width },
        { "support_object_xy_distance", xy_distance },
    });
    REQUIRE(support_points(print).size() > 1000);
    // The polygons are simplified to the slicing resolution on both sides of the gap.
    CHECK(smallest_support_clearance(print) > xy_distance - 0.05);
}
