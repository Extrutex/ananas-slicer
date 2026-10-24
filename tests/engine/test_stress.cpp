#include <catch2/catch_all.hpp>

#include "engine/Engine.hpp"
#include "engine/Stress.hpp"

#include <cmath>
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
