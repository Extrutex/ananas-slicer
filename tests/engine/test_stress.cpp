#include <catch2/catch_all.hpp>

#include "engine/Engine.hpp"
#include "engine/Stress.hpp"
#include "engine/StressApi.hpp"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>
#include <cstdio>

using namespace Slic3r;
using namespace Slic3r::engine;

namespace {

indexed_triangle_set bar(double lx, double ly, double lz)
{
    return its_make_cube(lx, ly, lz);
}

StressSelection box(const Vec3d& lo, const Vec3d& hi)
{
    StressSelection s;
    s.box = BoundingBoxf3(lo, hi);
    return s;
}

StressInput base(const indexed_triangle_set& mesh, double voxel)
{
    StressInput in;
    in.parts.push_back(mesh);
    in.voxel_size             = voxel;
    in.material.youngs_modulus = 2000.;
    in.material.poisson_ratio = 0.3;
    in.material.yield_strength = 40.;
    return in;
}

// The error code a call ends with, or none.
template<class F> std::optional<ErrorCode> error_of(F&& f)
{
    try {
        f();
    } catch (const EngineError& e) {
        return e.code();
    }
    return std::nullopt;
}

// A tension bar along `axis` (0 = x, 2 = z) of cross-section w x w and length L, held so that the stress state
// is exactly uniaxial: the end face may only slide in its plane, one node is held in the two other axes and a
// second only against the rotation about the bar.
StressInput tension_bar(int axis, double w, double L, double voxel, double force, double z_factor = 1.)
{
    Vec3d dim(w, w, w);
    dim[axis] = L;
    StressInput in = base(bar(dim.x(), dim.y(), dim.z()), voxel);
    in.material.z_factor = z_factor;
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;

    StressSupport face;
    face.where = box(Vec3d::Zero(), Vec3d(axis == 0 ? 0 : w, axis == 1 ? 0 : w, axis == 2 ? 0 : w));
    face.axes  = { false, false, false };
    face.axes[axis] = true;
    in.supports.push_back(face);

    StressSupport corner;
    corner.where = box(Vec3d::Zero(), Vec3d::Zero());
    corner.axes  = { true, true, true };
    corner.axes[axis] = false;
    in.supports.push_back(corner);

    StressSupport along_u; // the node at the end of the u edge, held against the rotation about the bar (v direction)
    Vec3d         at = Vec3d::Zero();
    at[u]            = w;
    along_u.where    = box(at, at);
    along_u.axes     = { false, false, false };
    along_u.axes[v]  = true;
    in.supports.push_back(along_u);

    StressLoad load;
    Vec3d      lo = Vec3d::Zero(), hi(w, w, w);
    lo[axis] = L;
    hi[axis] = L;
    load.where = box(lo, hi);
    load.force = Vec3d::Zero();
    load.force[axis] = force;
    in.loads.push_back(load);
    return in;
}

// The largest displacement of such a bar is at the far corner of the loaded end: the elongation plus the lateral
// contraction nu sigma w / E in both directions across, measured from the held corner.
double bar_max_displacement(double elongation, double sigma, double w, double E, double nu)
{
    const double lateral = nu * sigma / E * w;
    return std::sqrt(elongation * elongation + 2. * lateral * lateral);
}

// Tip deflection of a cantilever (length L along x, section w x w, load F in -z at the free end): Euler-Bernoulli
// plus the shear part of Timoshenko beam theory (shear coefficient 5/6).
double cantilever_analytic(double L, double w, double F, double E, double nu)
{
    const double I = w * w * w * w / 12., A = w * w, G = E / (2. * (1. + nu));
    return F * L * L * L / (3. * E * I) + F * L / (5. / 6. * G * A);
}

StressInput cantilever(double L, double w, double F, double voxel)
{
    StressInput in = base(bar(L, w, w), voxel);
    StressSupport clamp;
    clamp.where = box(Vec3d::Zero(), Vec3d(0, w, w));
    in.supports.push_back(clamp);
    StressLoad load;
    load.where = box(Vec3d(L, 0, 0), Vec3d(L, w, w));
    load.force = Vec3d(0, 0, -F);
    in.loads.push_back(load);
    return in;
}

} // namespace

TEST_CASE("Stress: a tension bar matches F L / (A E) and F / A", "[Engine][Stress]")
{
    const double w = 4., L = 10., F = 100., E = 2000.;
    for (double voxel : { 2., 1., 0.5 }) {
        const StressResult r = solve_stress(tension_bar(0, w, L, voxel, F));
        const double sigma = F / (w * w), delta = bar_max_displacement(F * L / (w * w * E), sigma, w, E, 0.3);
        std::printf("[stress] tension bar voxel %.2f: largest displacement %.6f (analytic %.6f), von Mises %.6f (analytic %.6f), "
                    "%zu iterations, residual %.2e\n",
                    voxel, r.max_displacement, delta, r.max_von_mises, sigma, r.iterations, r.residual);
        REQUIRE(r.converged);
        // The uniaxial state is in the trilinear element's space, so only the solver tolerance remains.
        CHECK(r.max_displacement == Catch::Approx(delta).epsilon(1e-4));
        CHECK(r.max_von_mises == Catch::Approx(sigma).epsilon(1e-4));
        CHECK(r.min_safety == Catch::Approx(40. / sigma).epsilon(1e-4));
        CHECK(r.reaction.x() == Catch::Approx(-F).epsilon(1e-4));
        CHECK(r.applied_force.x() == Catch::Approx(F).epsilon(1e-9));
    }
}

TEST_CASE("Stress: z_factor weakens a bar loaded across the layers", "[Engine][Stress]")
{
    const double w = 4., L = 10., F = 100., E = 2000.;
    const double factor  = 0.5;
    const StressResult r = solve_stress(tension_bar(2, w, L, 1., F, factor));
    const double sigma = F / (w * w), delta = bar_max_displacement(F * L / (w * w * factor * E), sigma, w, E, 0.3);
    std::printf("[stress] z bar, z_factor 0.5: largest displacement %.6f (analytic %.6f), von Mises %.6f, safety %.4f (analytic %.4f)\n",
                r.max_displacement, delta, r.max_von_mises, r.min_safety, factor * 40. / sigma);
    REQUIRE(r.converged);
    CHECK(r.max_displacement == Catch::Approx(delta).epsilon(1e-4));
    CHECK(r.max_von_mises == Catch::Approx(sigma).epsilon(1e-4));
    CHECK(r.min_safety == Catch::Approx(factor * 40. / sigma).epsilon(1e-4)); // the tensile limit across the layers governs

    // The same bar along x is not weakened.
    const StressResult in_plane = solve_stress(tension_bar(0, w, L, 1., F, factor));
    CHECK(in_plane.max_displacement == Catch::Approx(bar_max_displacement(F * L / (w * w * E), sigma, w, E, 0.3)).epsilon(1e-4));
    CHECK(in_plane.min_safety == Catch::Approx(40. / sigma).epsilon(1e-4));
}

TEST_CASE("Stress: cantilever deflection against beam theory, and its convergence", "[Engine][Stress]")
{
    const double L = 40., w = 4., F = 10.;
    const double analytic = cantilever_analytic(L, w, F, 2000., 0.3);
    double       previous_error = 1e9, error_at_half = 0.;
    for (double voxel : { 2., 1., 0.5 }) {
        const StressResult r = solve_stress(cantilever(L, w, F, voxel));
        const double error   = std::abs(r.max_displacement - analytic) / analytic;
        std::printf("[stress] cantilever voxel %.2f (%.0f cells over the height): tip %.4f mm (beam theory %.4f), error %.1f %%, "
                    "%zu iterations, max von Mises %.2f MPa (beam theory at the clamp %.2f)\n",
                    voxel, w / voxel, r.max_displacement, analytic, 100. * error, r.iterations, r.max_von_mises,
                    F * L * (w / 2.) / (w * w * w * w / 12.));
        REQUIRE(r.converged);
        CHECK(r.reaction.z() == Catch::Approx(F).epsilon(1e-4));
        CHECK(error < previous_error); // refining must bring the answer closer
        previous_error = error;
        if (voxel == 0.5)
            error_at_half = error;
        CHECK(r.max_displacement < analytic * 1.001); // trilinear elements lock in bending: never softer than the beam
    }
    // With 8 cells over the height the remaining gap is bending locking, the clamp's Poisson restraint and the shear model.
    CHECK(error_at_half < 0.03); // measured 1.4 %; coarser grids: 4.0 % (4 cells), 12.9 % (2 cells)
}

TEST_CASE("Stress: errors", "[Engine][Stress]")
{
    SECTION("no support")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.supports.clear();
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("no load")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.loads.clear();
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("empty model")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.parts.clear();
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
        in.parts.push_back(indexed_triangle_set());
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
    }
    SECTION("mesh with a hole in its surface")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.parts.front().indices.pop_back();
        REQUIRE(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
        try {
            solve_stress(in);
        } catch (const EngineError& e) {
            CHECK(std::string(e.what()).find("watertight") != std::string::npos);
        }
    }
    SECTION("model thinner than the voxel")
    {
        StressInput in = cantilever(10., 0.2, 1., 1.); // one cell wide and its centre is outside
        in.supports.front().where = box(Vec3d(-1, -1, -1), Vec3d(0, 1, 1));
        in.loads.front().where    = box(Vec3d(10, -1, -1), Vec3d(10, 1, 1));
        const auto code = error_of([&] { solve_stress(in); });
        CHECK(code.has_value()); // either no cell is solid or the selection finds no material
    }
    SECTION("support and load that select no material")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.supports.front().where = box(Vec3d(50, 50, 50), Vec3d(60, 60, 60));
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in = cantilever(10., 4., 1., 1.);
        in.loads.front().where = box(Vec3d(50, 50, 50), Vec3d(60, 60, 60));
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("a triangle that does not exist")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.supports.front().where.triangles = { 9999 };
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("a free piece without support")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        indexed_triangle_set second = bar(4, 4, 4);
        for (auto& v : second.vertices)
            v += Vec3f(30.f, 0.f, 0.f);
        in.parts.push_back(second);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
    }
    SECTION("supports that leave a rotation free do not converge")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.supports.front().where = box(Vec3d::Zero(), Vec3d::Zero()); // one node held: the bar can swing about it
        in.max_iterations         = 400;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
    }
    SECTION("bad material and voxel size")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.material.poisson_ratio = 0.5;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in = cantilever(10., 4., 1., 0.);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in = cantilever(10., 4., 1., 0.001);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams); // far too many cells
    }
}

TEST_CASE("Stress: selecting by triangles", "[Engine][Stress]")
{
    // The cube's triangles of the face x = 0 (all vertices at x = 0), found from the mesh itself.
    const indexed_triangle_set mesh = bar(10., 4., 4.);
    StressSelection            left, right;
    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        const auto& tri = mesh.indices[t];
        if (mesh.vertices[tri[0]].x() == 0.f && mesh.vertices[tri[1]].x() == 0.f && mesh.vertices[tri[2]].x() == 0.f)
            left.triangles.push_back(t);
        if (mesh.vertices[tri[0]].x() == 10.f && mesh.vertices[tri[1]].x() == 10.f && mesh.vertices[tri[2]].x() == 10.f)
            right.triangles.push_back(t);
    }
    REQUIRE(left.triangles.size() == 2);
    REQUIRE(right.triangles.size() == 2);

    StressInput by_triangles = base(mesh, 1.);
    by_triangles.supports.push_back({ left, { true, true, true } });
    by_triangles.loads.push_back({ right, Vec3d(0, 0, -5) });
    StressInput by_box = base(mesh, 1.);
    by_box.supports.push_back({ box(Vec3d::Zero(), Vec3d(0, 4, 4)), { true, true, true } });
    by_box.loads.push_back({ box(Vec3d(10, 0, 0), Vec3d(10, 4, 4)), Vec3d(0, 0, -5) });

    const StressResult a = solve_stress(by_triangles), b = solve_stress(by_box);
    std::printf("[stress] selection by triangles: tip %.6f mm, by box: tip %.6f mm\n", a.max_displacement, b.max_displacement);
    CHECK(a.max_displacement == Catch::Approx(b.max_displacement).epsilon(1e-6));
    CHECK(a.fixed_dofs == b.fixed_dofs);
}

namespace {

std::string message_of(const std::function<void()>& f)
{
    try {
        f();
    } catch (const EngineError& e) {
        return e.what();
    }
    return {};
}

bool has_warning(const StressResult& r, const std::string& part)
{
    return std::any_of(r.warnings.begin(), r.warnings.end(), [&](const std::string& w) { return w.find(part) != std::string::npos; });
}

} // namespace

TEST_CASE("Stress: limits on memory, runtime and input", "[Engine][Stress]")
{
    SECTION("cell limit, also for sizes that overflow")
    {
        StressInput in = cantilever(10., 4., 1., 0.5);
        in.max_cells   = 100;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in = cantilever(10., 4., 1., 1e-300); // size / voxel_size does not fit a size_t
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in = cantilever(10., 4., 1., 1.);
        in.max_cells = std::numeric_limits<size_t>::max(); // the server limit still holds
        for (auto& v : in.parts.front().vertices)
            v *= 1e6f;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("memory estimate")
    {
        StressInput in = cantilever(20., 4., 1., 0.5);
        in.max_bytes   = 100000;
        const std::string text = message_of([&] { solve_stress(in); });
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        CHECK(text.find("memory") != std::string::npos);
    }
    SECTION("supports that hold only some axes end quickly with an error")
    {
        StressInput in = cantilever(20., 4., 1., 0.5);
        in.supports.front().axes = { true, false, false }; // the face can still slide and the bar can rotate about x
        in.loads.front().force   = Vec3d(0, 0, -5);
        const auto start = std::chrono::steady_clock::now();
        std::string text = message_of([&] { solve_stress(in); });
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("[stress] partial axes: '%s' after %.3f s\n", text.c_str(), seconds);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
        CHECK(text.find("rigid-body") != std::string::npos);
        CHECK(seconds < 5.);
    }
    SECTION("rotation about the held line stays free")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.supports.front().where = box(Vec3d::Zero(), Vec3d(0, 0, 4)); // only the nodes of one edge
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::EngineError);
    }
    SECTION("a huge max_iterations is capped, a mechanism ends with an error")
    {
        // Two cubes joined only at a corner node, the far one loaded sideways: it can swing about the joint, which the
        // up-front check of the supports cannot see.
        StressInput in = cantilever(4., 4., 1., 1.);
        indexed_triangle_set second = bar(4, 4, 4);
        for (auto& v : second.vertices)
            v += Vec3f(4.f, 4.f, 4.f);
        in.parts.push_back(second);
        in.loads.front().where = box(Vec3d(8, 4, 4), Vec3d(8, 8, 8));
        in.loads.front().force = Vec3d(0, 0, 1);
        in.max_iterations      = std::numeric_limits<size_t>::max();
        const auto start       = std::chrono::steady_clock::now();
        const std::string text = message_of([&] { solve_stress(in); });
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("[stress] hinged piece: '%s' after %.2f s\n", text.c_str(), seconds);
        // The cap on the iterations (20000) or the solver's own stop ends it: with the specific message, and soon.
        CHECK((text.find("did not converge") != std::string::npos || text.find("stagnated") != std::string::npos));
        CHECK(seconds < 60.);
        const StressResult ok = solve_stress([&] {
            StressInput fine = cantilever(10., 4., 1., 1.);
            fine.max_iterations = std::numeric_limits<size_t>::max();
            return fine;
        }());
        CHECK(ok.converged);
    }
    SECTION("wall-clock limit")
    {
        StressInput in = cantilever(20., 4., 1., 0.5);
        in.max_seconds = 1e-9;
        const std::string text = message_of([&] { solve_stress(in); });
        CHECK(text.find("time limit") != std::string::npos);
        in.max_seconds = 0.;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("vertices that are not finite")
    {
        for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
            StressInput in = cantilever(10., 4., 1., 1.);
            in.parts.front().vertices[3].y() = bad;
            CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        }
        StressInput in = cantilever(10., 4., 1., 1.);
        in.parts.front().indices[0][0] = 9999;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("chunk size")
    {
        const StressResult r = solve_stress(cantilever(10., 4., 1., 1.)); // 10 x 4 x 4 cells
        CHECK_NOTHROW(stress_chunk_json(1, r, 0, 3));
        CHECK(error_of([&] { stress_chunk_json(1, r, 0, 3, 25 * 100); }) == ErrorCode::InvalidParams);          // 160 cells
        CHECK_NOTHROW(stress_chunk_json(1, r, 0, 1, 25 * 100));                                                   // 80 cells
        CHECK(message_of([&] { stress_chunk_json(1, r, 0, 3, 25 * 100); }).find("at most 2 fit") != std::string::npos);
        CHECK(error_of([&] { stress_chunk_json(1, r, 0, 0, 25 * 10); }) == ErrorCode::InvalidParams);            // one layer is 40 cells
    }
}

TEST_CASE("Stress: warnings", "[Engine][Stress]")
{
    SECTION("a solid, thick bar has none")
    {
        const StressResult r = solve_stress(cantilever(24., 8., 1., 1.));
        CHECK(r.warnings.empty());
    }
    SECTION("thin walls")
    {
        const StressResult r = solve_stress(cantilever(24., 3., 1., 1.));
        CHECK(has_warning(r, "thinner than 4 cells"));
    }
    SECTION("pieces touching at a corner")
    {
        StressInput in = cantilever(4., 4., 1., 1.);
        indexed_triangle_set second = bar(4, 4, 4);
        for (auto& v : second.vertices)
            v += Vec3f(4.f, 4.f, 4.f);
        in.parts.push_back(second);
        in.loads.front().where = box(Vec3d(0, 0, 4), Vec3d(4, 4, 4)); // the load sits on the first piece
        in.loads.front().force = Vec3d(0, 0, 1);
        const StressResult r = solve_stress(in);
        CHECK(has_warning(r, "edge or corner"));
    }
    SECTION("volume lost to the voxel grid")
    {
        StressInput in = cantilever(10., 4., 1., 3.); // 4 mm in cells of 3 mm: one cell, 3 of 4 mm
        in.supports.front().where = box(Vec3d(-1, -1, -1), Vec3d(0, 5, 5));
        in.loads.front().where    = box(Vec3d(9, -1, -1), Vec3d(11, 5, 5));
        const StressResult r = solve_stress(in);
        std::printf("[stress] coarse grid warnings: %zu\n", r.warnings.size());
        CHECK(has_warning(r, "volume"));
    }
}

namespace {

// The bar of tension_bar() with the load given by the triangles of its far end face (x == L) instead of a box.
StressInput tension_bar_by_triangles(double w, double L, double voxel, double force)
{
    StressInput in = tension_bar(0, w, L, voxel, force);
    StressSelection far_end;
    const auto&     mesh = in.parts.front();
    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        const auto& tri = mesh.indices[t];
        if (mesh.vertices[tri[0]].x() == float(L) && mesh.vertices[tri[1]].x() == float(L) && mesh.vertices[tri[2]].x() == float(L))
            far_end.triangles.push_back(t);
    }
    in.loads.front().where = far_end;
    return in;
}

} // namespace

TEST_CASE("Stress: a grid layer larger than one result chunk is refused before solving", "[Engine][Stress]")
{
    // 600 x 600 cells in a layer: 360 000 * 25 bytes = 9 MB, more than the 8 MB of one chunk; 720 000 cells in total, so
    // the cell limit alone would let it pass.
    StressInput in = base(bar(600., 600., 2.), 1.);
    in.supports.push_back({ box(Vec3d(0, 0, 0), Vec3d(0, 600, 2)), { true, true, true } });
    in.loads.push_back({ box(Vec3d(600, 0, 0), Vec3d(600, 600, 2)), Vec3d(0, 0, -1) });
    const auto        start = std::chrono::steady_clock::now();
    const std::string text  = message_of([&] { solve_stress(in); });
    const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("[stress] layer over the chunk limit: '%s' after %.3f s\n", text.c_str(), seconds);
    CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    CHECK(text.find("voxel_size") != std::string::npos);
    CHECK(text.find("layer") != std::string::npos);
    CHECK(seconds < 5.);

    // 570 x 570 = 324 900 cells * 25 = 8.1 MB fit; checked on the layer arithmetic of the chunk call itself.
    StressResult r;
    r.size = { 570, 570, 1 };
    r.solid.assign(570 * 570, 0);
    r.von_mises.assign(570 * 570, 0.f);
    r.safety.assign(570 * 570, 0.f);
    r.displacement.assign(570 * 570, 0.f);
    r.displacement_vec.assign(570 * 570 * 3, 0.f);
    CHECK_NOTHROW(stress_chunk_json(1, r, 0, 0));
}

TEST_CASE("Stress: huge and tiny forces", "[Engine][Stress]")
{
    const StressResult one = solve_stress(cantilever(10., 4., 1., 1.));
    REQUIRE(one.converged);

    for (double huge : { 1e200, 1e300, 1e13 }) {
        StressInput in = cantilever(10., 4., huge, 1.);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("several loads that add up beyond the limit")
    {
        StressInput in = cantilever(10., 4., 6e11, 1.);
        in.loads.push_back(in.loads.front());
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in.loads.back().force = -in.loads.front().force; // opposite loads do not cancel the check
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    for (double F : { 1e11, 1e-200 }) {
        const StressResult r = solve_stress(cantilever(10., 4., F, 1.));
        std::printf("[stress] force %g N: converged %d, largest displacement %g (per N: %g, one newton: %g), reaction z %g\n", F,
                    int(r.converged), r.max_displacement, r.max_displacement / F, one.max_displacement, r.reaction.z());
        REQUIRE(r.converged);
        CHECK(r.max_displacement / F == Catch::Approx(one.max_displacement).epsilon(1e-3));
        CHECK(r.reaction.z() / F == Catch::Approx(1.).epsilon(1e-3));
        CHECK(r.applied_force.z() / F == Catch::Approx(-1.).epsilon(1e-9));
    }
}

TEST_CASE("Stress: limits on supports, loads, selections and the work before the solver", "[Engine][Stress]")
{
    SECTION("number of supports and loads")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        const StressSupport support = in.supports.front();
        in.supports.assign(STRESS_MAX_REGIONS, support);
        CHECK_NOTHROW(solve_stress(in));
        in.supports.push_back(support);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        in = cantilever(10., 4., 1., 1.);
        const StressLoad load = in.loads.front();
        in.loads.assign(STRESS_MAX_REGIONS + 1, load);
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("selected triangles: duplicates count once, different ones are capped")
    {
        const indexed_triangle_set mesh = bar(10., 4., 4.);
        StressInput                in   = base(mesh, 1.);
        StressSelection            left, right;
        for (size_t t = 0; t < mesh.indices.size(); ++t) {
            const auto& tri = mesh.indices[t];
            if (mesh.vertices[tri[0]].x() == 0.f && mesh.vertices[tri[1]].x() == 0.f && mesh.vertices[tri[2]].x() == 0.f)
                left.triangles.push_back(t);
            if (mesh.vertices[tri[0]].x() == 10.f && mesh.vertices[tri[1]].x() == 10.f && mesh.vertices[tri[2]].x() == 10.f)
                right.triangles.push_back(t);
        }
        REQUIRE(left.triangles.size() == 2);
        in.supports.push_back({ left, { true, true, true } });
        in.loads.push_back({ right, Vec3d(0, 0, -5) });
        const StressResult single = solve_stress(in);

        StressInput repeated = in;
        for (int copy = 0; copy < 50; ++copy) {
            repeated.supports.front().where.triangles.insert(repeated.supports.front().where.triangles.end(), left.triangles.begin(), left.triangles.end());
            repeated.loads.front().where.triangles.insert(repeated.loads.front().where.triangles.end(), right.triangles.begin(), right.triangles.end());
        }
        repeated.max_selection_triangles = 2; // two different triangles per selection are allowed, however often listed
        const StressResult many = solve_stress(repeated);
        CHECK(many.max_displacement == Catch::Approx(single.max_displacement).epsilon(1e-9));
        CHECK(many.applied_force.z() == Catch::Approx(-5.).epsilon(1e-9));

        in.max_selection_triangles = 1;
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
    }
    SECTION("surface crossings are limited by the memory budget")
    {
        StressInput in = cantilever(20., 4., 1., 0.5); // 40 x 8 x 8 = 2560 cells, about 640 crossings
        const double cells = 2560.;
        in.max_bytes = size_t(49. * cells + 48. * 100.); // room for 100 crossings
        const std::string text = message_of([&] { solve_stress(in); });
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        CHECK(text.find("crossings") != std::string::npos);
        in.max_bytes = size_t(1) << 30;
        CHECK_NOTHROW(solve_stress(in));
    }
    SECTION("the memory estimate counts the work before the voxelization")
    {
        const indexed_triangle_set sphere = its_make_sphere(20., 0.05); // many triangles, a coarse grid
        REQUIRE(sphere.indices.size() > 10000);
        StressInput in = base(sphere, 10.);
        in.supports.push_back({ box(Vec3d(-30, -30, -30), Vec3d(30, 30, -19)), { true, true, true } });
        in.loads.push_back({ box(Vec3d(-30, -30, 19), Vec3d(30, 30, 30)), Vec3d(0, 0, -1) });
        in.max_bytes = 200000; // far below what welding 10 000+ triangles needs
        const std::string text = message_of([&] { solve_stress(in); });
        CHECK(error_of([&] { solve_stress(in); }) == ErrorCode::InvalidParams);
        CHECK(text.find("memory") != std::string::npos);
    }
    SECTION("the time limit also holds before the solver")
    {
        StressInput in = cantilever(20., 4., 1., 0.5);
        in.max_seconds = 1e-9;
        const std::string text = message_of([&] { solve_stress(in); });
        std::printf("[stress] time limit at once: '%s'\n", text.c_str());
        CHECK(text.find("time limit") != std::string::npos);
        CHECK(text.find("while solving") == std::string::npos); // it ended in the input check, not in the solver

        // A mesh whose checking alone takes far longer than the limit ends in time, not after the whole preparation.
        const indexed_triangle_set sphere = its_make_sphere(20., 0.004);
        std::printf("[stress] sphere with %zu triangles\n", sphere.indices.size());
        StressInput big = base(sphere, 10.);
        big.supports.push_back({ box(Vec3d(-30, -30, -30), Vec3d(30, 30, -19)), { true, true, true } });
        big.loads.push_back({ box(Vec3d(-30, -30, 19), Vec3d(30, 30, 30)), Vec3d(0, 0, -1) });
        for (double limit : { 0.02, 0.3, 0.4, 0.45 }) { // the clock runs out in different phases of the preparation
            big.max_seconds = limit;
            const auto        start = std::chrono::steady_clock::now();
            const std::string late  = message_of([&] { solve_stress(big); });
            const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("[stress] time limit %.2f s on a big mesh: '%s' after %.3f s\n", limit, late.c_str(), seconds);
            CHECK((late.empty() || late.find("time limit") != std::string::npos)); // a fast run may finish within the limit
            CHECK(late.find("while solving") == std::string::npos);
            CHECK(seconds < limit + 3.);
            if (limit <= 0.3)
                CHECK(late.find("time limit") != std::string::npos); // the preparation alone takes longer than this
        }
    }
}

TEST_CASE("Stress: a face between two grid planes loads only the outer layer", "[Engine][Stress]")
{
    // The bar is 10.6 long, the grid has cells of 1: the voxel bar ends at x = 11. Nodes of the planes x = 10 and x = 11 are
    // both within reach of the face at x = 10.6, but only the plane x = 11 is the outer layer; the side faces between x = 10
    // and x = 11 must not take any of the load.
    const double w = 4., L = 10.6, F = 100., E = 2000.;
    const StressResult by_triangles = solve_stress(tension_bar_by_triangles(w, L, 1., F));
    const StressResult by_box       = solve_stress(tension_bar(0, w, L, 1., F));
    const double       voxel_length = 11.;
    const double       sigma = F / (w * w), delta = bar_max_displacement(F * voxel_length / (w * w * E), sigma, w, E, 0.3);
    std::printf("[stress] face at x=10.6: by triangles %.6f mm, by box %.6f mm, analytic %.6f mm; von Mises %.6f (analytic %.6f); "
                "reaction %.6f\n",
                by_triangles.max_displacement, by_box.max_displacement, delta, by_triangles.max_von_mises, sigma, by_triangles.reaction.x());
    REQUIRE(by_triangles.converged);
    CHECK(by_triangles.max_displacement == Catch::Approx(delta).epsilon(1e-4));
    CHECK(by_triangles.max_displacement == Catch::Approx(by_box.max_displacement).epsilon(1e-9));
    CHECK(by_triangles.max_von_mises == Catch::Approx(sigma).epsilon(1e-4));
    CHECK(by_triangles.applied_force.x() == Catch::Approx(F).epsilon(1e-9));
    CHECK(by_triangles.reaction.x() == Catch::Approx(-F).epsilon(1e-4));
    CHECK(std::abs(by_triangles.reaction.y()) < 1e-6 * F);
    CHECK(std::abs(by_triangles.reaction.z()) < 1e-6 * F);
}

TEST_CASE("Stress: rigid-body check is judged per piece and does not depend on position or size", "[Engine][Stress]")
{
    SECTION("two small, correctly held pieces far apart")
    {
        // Two single cells of 4 mm, 1 000 000 mm apart; each is held on a face. Relative to the whole model's extent the
        // held nodes of one piece lie within 4e-6, which a global scale would take for a collinear support.
        StressInput in = base(bar(4., 4., 4.), 4.);
        indexed_triangle_set second = bar(4., 4., 4.);
        for (auto& v : second.vertices)
            v += Vec3f(1000000.f, 0.f, 0.f);
        in.parts.push_back(second);
        in.supports.push_back({ box(Vec3d(0, 0, 0), Vec3d(0, 4, 4)), { true, true, true } });
        in.supports.push_back({ box(Vec3d(1000000, 0, 0), Vec3d(1000000, 4, 4)), { true, true, true } });
        in.loads.push_back({ box(Vec3d(4, 0, 0), Vec3d(4, 4, 4)), Vec3d(0, 0, -1) });
        const StressResult r = solve_stress(in);
        CHECK(r.converged);
        CHECK(r.reaction.z() == Catch::Approx(1.).epsilon(1e-4));
    }
    SECTION("the model scaled by 1000 and by 0.001 gives the same result")
    {
        const StressResult reference = solve_stress(cantilever(10., 4., 10., 1.));
        REQUIRE(reference.converged);
        for (double s : { 1000., 0.001 }) {
            const StressResult r = solve_stress(cantilever(10. * s, 4. * s, 10., 1. * s));
            std::printf("[stress] scale %g: displacement * s %.6f (reference %.6f), von Mises * s^2 %.6f (reference %.6f)\n", s,
                        r.max_displacement * s, reference.max_displacement, r.max_von_mises * s * s, reference.max_von_mises);
            REQUIRE(r.converged);
            CHECK(r.max_displacement * s == Catch::Approx(reference.max_displacement).epsilon(1e-3));
            CHECK(r.max_von_mises * s * s == Catch::Approx(reference.max_von_mises).epsilon(1e-3));
        }
    }
    SECTION("three single points held in all axes are enough, three on a line are not")
    {
        StressInput in = cantilever(10., 4., 1., 1.);
        in.supports.clear();
        for (const Vec3d& at : { Vec3d(0, 0, 0), Vec3d(0, 4, 0), Vec3d(0, 0, 4) })
            in.supports.push_back({ box(at, at), { true, true, true } });
        const StressResult r = solve_stress(in);
        CHECK(r.converged);
        CHECK(r.fixed_dofs == 9);

        in.supports.clear();
        for (const Vec3d& at : { Vec3d(0, 0, 0), Vec3d(0, 2, 0), Vec3d(0, 4, 0) })
            in.supports.push_back({ box(at, at), { true, true, true } });
        const std::string text = message_of([&] { solve_stress(in); });
        CHECK(text.find("rigid-body") != std::string::npos);
        CHECK(text.find("1 of 6") != std::string::npos); // the rotation about the line
    }
}

TEST_CASE("Stress: closed meshes of every kind never count a ray twice on a shared edge", "[Engine][Stress]")
{
    // Cube (its faces are split along diagonals), sphere, cylinder, cone and prism, on grids that put vertices and edges
    // on and next to grid lines. A ray through a shared edge counted in both or in neither triangle would make the
    // crossings odd and the mesh look open.
    struct Shape
    {
        const char*         name;
        indexed_triangle_set mesh;
    };
    const Shape shapes[] = { { "cube", its_make_cube(10., 10., 10.) },
                             { "sphere", its_make_sphere(5., PI / 16.) },
                             { "cylinder", its_make_cylinder(4., 10., PI / 12.) },
                             { "cone", its_make_cone(4., 10., PI / 12.) },
                             { "prism", its_make_prism(8.f, 6.f, 9.f) },
                             { "pyramid", its_make_pyramid(8.f, 9.f) } };
    size_t      runs = 0, odd = 0;
    for (const Shape& shape : shapes) {
        BoundingBoxf3 bb;
        for (const Vec3f& v : shape.mesh.vertices)
            bb.merge(v.cast<double>());
        for (double voxel : { 0.25, 0.3, 0.5, 0.7, 1., 1.25, 2., 2.5 }) {
            StressInput in = base(shape.mesh, voxel);
            in.supports.push_back({ box(bb.min - Vec3d::Constant(1.), Vec3d(bb.max.x() + 1., bb.max.y() + 1., bb.min.z() + 1.)), { true, true, true } });
            in.loads.push_back({ box(Vec3d(bb.min.x() - 1., bb.min.y() - 1., bb.max.z() - 1.), bb.max + Vec3d::Constant(1.)), Vec3d(0, 0, -1) });
            in.max_iterations = 5; // only the voxelization is on trial
            const std::string text = message_of([&] { solve_stress(in); });
            ++runs;
            if (text.find("watertight") != std::string::npos) {
                ++odd;
                std::printf("[stress] %s voxel %.2f: %s\n", shape.name, voxel, text.c_str());
            }
        }
    }
    std::printf("[stress] watertight meshes voxelized: %zu runs, %zu reported as open\n", runs, odd);
    CHECK(odd == 0);
}
