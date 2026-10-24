// Der Multigrid-Vorkonditionierer des Spannungslösers (analyze.stress) gegen den Jacobi-Vorkonditionierer: gleiche
// Ergebnisse auf kleinen Fällen, Obergrenzen für die Iterationszahl als Regressionsschutz, Rückfall auf Jacobi,
// ungerade und nicht quadratische Gitter, leere Grobzellen, dünne Wände, Anisotropie, kleine Lager, mehrere Teile.
// Die Messungen an großen Gittern (Minuten bei Jacobi) laufen nur mit ANANAS_STRESS_FULL=1. Messwerte und Deutung stehen
// in docs/HLSD/stress-validierung.md.
#include <catch2/catch_all.hpp>

#include "engine/Engine.hpp"
#include "engine/Stress.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::engine;

namespace {

indexed_triangle_set box_mesh(const Vec3d& lo, const Vec3d& size)
{
    indexed_triangle_set its = its_make_cube(size.x(), size.y(), size.z());
    for (Vec3f& v : its.vertices)
        v += lo.cast<float>();
    return its;
}

// Hohlkörper: Außenquader und ein nach innen gewendeter Hohlraum im selben Netz.
indexed_triangle_set hollow_box(double outer, double wall)
{
    indexed_triangle_set its = box_mesh(Vec3d::Zero(), Vec3d(outer, outer, outer));
    indexed_triangle_set cavity = box_mesh(Vec3d::Constant(wall), Vec3d::Constant(outer - 2. * wall));
    its_flip_triangles(cavity);
    its_merge(its, cavity);
    return its;
}

StressSelection box(const Vec3d& lo, const Vec3d& hi)
{
    StressSelection s;
    s.box = BoundingBoxf3(lo, hi);
    return s;
}

StressInput base(double voxel)
{
    StressInput in;
    in.voxel_size              = voxel;
    in.material.youngs_modulus = 2000.;
    in.material.poisson_ratio  = 0.3;
    in.material.yield_strength = 40.;
    return in;
}

void clamp(StressInput& in, const Vec3d& lo, const Vec3d& hi)
{
    StressSupport s;
    s.where = box(lo, hi);
    in.supports.push_back(s);
}

void load(StressInput& in, const Vec3d& lo, const Vec3d& hi, const Vec3d& force)
{
    StressLoad l;
    l.where = box(lo, hi);
    l.force = force;
    in.loads.push_back(l);
}

// Balken längs x, an x = 0 eingespannt, am freien Ende nach unten belastet.
StressInput cantilever(double L, double w, double h, double voxel)
{
    StressInput in = base(voxel);
    in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(L, w, h)));
    clamp(in, Vec3d::Zero(), Vec3d(0, w, h));
    load(in, Vec3d(L, 0, 0), Vec3d(L, w, h), Vec3d(0, 0, -50.));
    return in;
}

struct Pair
{
    StressResult mg, jacobi;
    double       mg_seconds{ 0. }, jacobi_seconds{ 0. };
    double       displacement_diff{ 0. }, stress_diff{ 0. }, max_stress_diff{ 0. };
};

StressResult timed(StressInput in, StressPreconditioner which, double& seconds)
{
    in.preconditioner = which;
    const auto start  = std::chrono::steady_clock::now();
    StressResult r    = solve_stress(in);
    seconds           = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return r;
}

// Beide Vorkonditionierer auf demselben Fall; Unterschied der Felder bezogen auf ihr Maximum.
Pair both(const char* name, const StressInput& in, bool run_jacobi = true)
{
    Pair out;
    out.mg = timed(in, StressPreconditioner::Multigrid, out.mg_seconds);
    if (run_jacobi) {
        out.jacobi = timed(in, StressPreconditioner::Jacobi, out.jacobi_seconds);
        double du = 0., dv = 0., umax = 0., vmax = 0.;
        for (size_t i = 0; i < out.mg.displacement.size(); ++i) {
            du   = std::max(du, std::abs(double(out.mg.displacement[i]) - double(out.jacobi.displacement[i])));
            dv   = std::max(dv, std::abs(double(out.mg.von_mises[i]) - double(out.jacobi.von_mises[i])));
            umax = std::max(umax, double(out.jacobi.displacement[i]));
            vmax = std::max(vmax, double(out.jacobi.von_mises[i]));
        }
        out.displacement_diff = du / umax;
        out.stress_diff       = dv / vmax;
        out.max_stress_diff   = std::abs(out.mg.max_von_mises - out.jacobi.max_von_mises) / out.jacobi.max_von_mises;
    }
    std::printf("[Multigrid] %-44s %9zu Zellen | Jacobi %5zu It. %7.2f s | Multigrid %3zu It. %6.2f s (%zu Ebenen) | Unterschied u %.1e, sigma %.1e, max sigma %.1e\n",
                name, out.mg.solid_cells, out.jacobi.iterations, out.jacobi_seconds, out.mg.iterations, out.mg_seconds, out.mg.multigrid_levels,
                out.displacement_diff, out.stress_diff, out.max_stress_diff);
    return out;
}

bool full_run() { return std::getenv("ANANAS_STRESS_FULL") != nullptr; }

bool has_warning(const StressResult& r, const std::string& part)
{
    return std::any_of(r.warnings.begin(), r.warnings.end(), [&](const std::string& w) { return w.find(part) != std::string::npos; });
}

} // namespace

TEST_CASE("Stress: multigrid gives the Jacobi result on regular, odd, thin and anisotropic bodies", "[Engine][Stress][Multigrid]")
{
    std::vector<std::pair<const char*, StressInput>> cases;
    cases.emplace_back("Kragarm 24x4x4 (gerade)", cantilever(24., 4., 4., 1.));
    cases.emplace_back("Kragarm 33x17x9 (ungerade, nx!=ny!=nz)", cantilever(33., 17., 9., 1.));
    cases.emplace_back("Kragarm 30.5x7.5x5.5 bei 0,5 mm (ungerade)", cantilever(30.5, 7.5, 5.5, 0.5));
    cases.emplace_back("Stab 200x2x2 (zwei Zellen Querschnitt)", cantilever(200., 2., 2., 1.));
    cases.emplace_back("Platte 60x40x1 (eine Zelle dick)", cantilever(60., 40., 1., 1.));
    cases.emplace_back("Stab 3x3x101 (Gitter 3x3x101)", cantilever(101., 3., 3., 1.));
    {
        StressInput in = cantilever(40., 8., 8., 1.);
        in.material.z_factor = 0.3;
        cases.emplace_back("Kragarm 40x8x8, z_factor 0,3", in);
    }
    {
        StressInput in = base(1.); // Lager nur auf 2x2 Zellen an einer Ecke der Stirnfläche
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(40., 8., 8.)));
        clamp(in, Vec3d::Zero(), Vec3d(0, 2, 2));
        load(in, Vec3d(40, 0, 0), Vec3d(40, 8, 8), Vec3d(0, 0, -5.));
        cases.emplace_back("Lager nur an 2x2 Zellen", in);
    }
    {
        StressInput in = base(1.); // drei Punktlager an Knoten mit ungeradem Index, kein Flächenlager
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(31., 7., 5.)));
        const auto pin = [&](const Vec3d& at, std::array<bool, 3> axes) {
            StressSupport sp;
            sp.where = box(at, at);
            sp.axes  = axes;
            in.supports.push_back(sp);
        };
        pin(Vec3d(0, 0, 0), { true, true, true });
        pin(Vec3d(31, 0, 0), { false, true, true });
        pin(Vec3d(0, 7, 0), { false, false, true });
        load(in, Vec3d(14, 2, 5), Vec3d(17, 5, 5), Vec3d(0, 0, -20.));
        cases.emplace_back("drei Punktlager an ungeraden Knoten", in);
    }
    {
        StressInput in = base(1.); // Hohlkasten, Wände 2 Zellen dick, Grobzellen im Hohlraum leer
        in.parts.push_back(hollow_box(24., 2.));
        clamp(in, Vec3d::Zero(), Vec3d(24, 24, 0));
        load(in, Vec3d(0, 0, 24), Vec3d(24, 24, 24), Vec3d(0, 10., -20.));
        cases.emplace_back("Hohlkasten 24, Wand 2 Zellen", in);
    }
    {
        StressInput in = base(1.); // Wand nur 1 Zelle dick
        in.parts.push_back(hollow_box(16., 1.));
        clamp(in, Vec3d::Zero(), Vec3d(16, 16, 0));
        load(in, Vec3d(0, 0, 16), Vec3d(16, 16, 16), Vec3d(0, 5., -10.));
        cases.emplace_back("Hohlkasten 16, Wand 1 Zelle", in);
    }
    {
        StressInput in = base(1.); // zwei getrennte Teile, jedes für sich gelagert und belastet
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(30., 6., 6.)));
        in.parts.push_back(box_mesh(Vec3d(0, 12, 0), Vec3d(30., 6., 6.)));
        clamp(in, Vec3d::Zero(), Vec3d(0, 6, 6));
        clamp(in, Vec3d(0, 12, 0), Vec3d(0, 18, 6));
        load(in, Vec3d(30, 0, 0), Vec3d(30, 6, 6), Vec3d(0, 0, -20.));
        load(in, Vec3d(30, 12, 0), Vec3d(30, 18, 6), Vec3d(0, 0, 20.));
        cases.emplace_back("zwei getrennte Balken", in);
    }
    {
        StressInput in = base(1.); // L-Form aus zwei Teilen: viele leere Grobzellen
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(31., 7., 5.)));
        in.parts.push_back(box_mesh(Vec3d(0, 7, 0), Vec3d(7., 24., 5.)));
        clamp(in, Vec3d(0, 24, 0), Vec3d(7, 31, 0));
        StressSupport side;
        side.where = box(Vec3d(0, 31, 0), Vec3d(7, 31, 5));
        in.supports.push_back(side);
        load(in, Vec3d(31, 0, 0), Vec3d(31, 7, 5), Vec3d(0, 15., 0));
        cases.emplace_back("L-Form 31x31 (ungerade, leere Grobzellen)", in);
    }
    for (const auto& [name, in] : cases) {
        DYNAMIC_SECTION(name)
        {
            const Pair p = both(name, in);
            REQUIRE(p.mg.converged);
            REQUIRE(p.jacobi.converged);
            CHECK(p.mg.preconditioner == StressPreconditioner::Multigrid);
            CHECK(p.mg.multigrid_levels >= 1);
            CHECK(p.jacobi.preconditioner == StressPreconditioner::Jacobi);
            // Dasselbe Residuum 1e-6 liefert Felder, die auf etwa 1e-4 des Maximums übereinstimmen.
            CHECK(p.displacement_diff < 1e-4);
            CHECK(p.stress_diff < 1e-4);
            CHECK(p.max_stress_diff < 1e-4);
            CHECK(p.mg.applied_force.z() == Catch::Approx(p.jacobi.applied_force.z()).margin(1e-9));
            CHECK((p.mg.reaction - p.jacobi.reaction).norm() < 1e-3); // N; die Lasten liegen bei 5 bis 50 N
            // Regressionsschutz, großzügig (gemessen höchstens etwa 60): Jacobi braucht hier ein Mehrfaches.
            CHECK(p.mg.iterations < 150);
        }
    }
}

TEST_CASE("Stress: multigrid keeps slender and fine grids at few iterations", "[Engine][Stress][Multigrid]")
{
    // Jacobi braucht beim Balken 96 x 6 x 6 mm: 1 184 (1,0 mm), 2 458 (0,5 mm), 5 669 (0,25 mm) Iterationen.
    for (double voxel : { 1., 0.5 }) {
        DYNAMIC_SECTION("Balken 96x6x6, Voxel " << voxel)
        {
            const Pair p = both("Kragarm 96x6x6", cantilever(96., 6., 6., voxel), false);
            REQUIRE(p.mg.converged);
            CHECK(p.mg.iterations < 100);
        }
    }
    if (full_run()) {
        const Pair p = both("Kragarm 96x6x6, Voxel 0,25", cantilever(96., 6., 6., 0.25), false);
        REQUIRE(p.mg.converged);
        CHECK(p.mg.iterations < 100); // die Messlatte der Stufe: vorher 5 669
    }
}

TEST_CASE("Stress: multigrid falls back to Jacobi with a warning when it fails", "[Engine][Stress][Multigrid]")
{
    const StressInput good = cantilever(24., 4., 4., 1.);
    StressInput       jac  = good;
    jac.preconditioner     = StressPreconditioner::Jacobi;
    const StressResult ref = solve_stress(jac);
    REQUIRE(ref.converged);
    CHECK(ref.warnings.empty());

    SECTION("the hierarchy cannot be built")
    {
        StressInput in = good;
        in.fault_for_test = 1;
        const StressResult r = solve_stress(in);
        CHECK(r.converged);
        CHECK(r.preconditioner == StressPreconditioner::Jacobi);
        CHECK(r.multigrid_levels == 0);
        CHECK(has_warning(r, "multigrid"));
        CHECK(r.max_displacement == Catch::Approx(ref.max_displacement).epsilon(1e-9));
        CHECK(r.iterations == ref.iterations);
    }
    SECTION("a cycle goes wrong in the middle of the run")
    {
        StressInput in = good;
        in.fault_for_test = 2;
        const StressResult r = solve_stress(in);
        CHECK(r.converged);
        CHECK(r.preconditioner == StressPreconditioner::Jacobi);
        CHECK(has_warning(r, "multigrid"));
        CHECK(r.max_displacement == Catch::Approx(ref.max_displacement).epsilon(1e-4));
        CHECK(r.max_von_mises == Catch::Approx(ref.max_von_mises).epsilon(1e-4));
    }
    SECTION("the memory budget leaves no room for the hierarchy")
    {
        // Das Jacobi-Gerüst passt, die Hierarchie nicht: der Löser rechnet mit Jacobi und sagt es.
        StressInput in = good;
        in.max_bytes = 0;
        // Ein Budget von 0 verweigert die Analyse schon vorher; gesucht ist die Grenze, an der nur noch Jacobi passt.
        size_t low = 1, high = size_t(1) << 30;
        while (high - low > 1024) {
            const size_t mid = (low + high) / 2;
            in.max_bytes     = mid;
            bool ok          = true;
            try {
                solve_stress(in);
            } catch (const EngineError&) {
                ok = false;
            }
            (ok ? high : low) = mid;
        }
        in.max_bytes = high;
        const StressResult r = solve_stress(in);
        CHECK(r.converged);
        CHECK(r.preconditioner == StressPreconditioner::Jacobi);
        CHECK(has_warning(r, "memory"));
    }
}

TEST_CASE("Stress: multigrid keeps the time limit and the iteration limit", "[Engine][Stress][Multigrid]")
{
    StressInput in = cantilever(96., 6., 6., 0.5);
    SECTION("max_seconds")
    {
        in.max_seconds = 0.001;
        CHECK_THROWS_AS(solve_stress(in), EngineError);
    }
    SECTION("max_iterations")
    {
        in.max_iterations = 3;
        CHECK_THROWS_AS(solve_stress(in), EngineError);
    }
}

TEST_CASE("Stress: multigrid on a solid cube of a million cells", "[Engine][Stress][Multigrid][.]")
{
    // Nur mit ANANAS_STRESS_FULL=1 und ausdrücklich angefordert (Jacobi braucht hier Minuten).
    if (!full_run())
        SKIP("nur mit ANANAS_STRESS_FULL");
    StressInput in = base(1.);
    const double a = std::getenv("ANANAS_CUBE") ? std::atof(std::getenv("ANANAS_CUBE")) : 100.;
    in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(a, a, a)));
    clamp(in, Vec3d::Zero(), Vec3d(a, a, 0));
    load(in, Vec3d(0, 0, a), Vec3d(a, a, a), Vec3d(30000., 0, -30000.));
    // ANANAS_CUBE_ONLY=jacobi|multigrid rechnet nur mit einem Vorkonditionierer (Speichermessung mit /usr/bin/time -l).
    const char*  only = std::getenv("ANANAS_CUBE_ONLY");
    if (only) {
        double seconds = 0.;
        const StressResult r = timed(in, std::string(only) == "jacobi" ? StressPreconditioner::Jacobi : StressPreconditioner::Multigrid, seconds);
        std::printf("[Multigrid] massiver Wuerfel %zu Zellen, %s: %zu Iterationen, %.2f s, Residuum %.1e\n", r.solid_cells, only, r.iterations, seconds, r.residual);
        CHECK(r.converged);
        return;
    }
    const Pair p = both("massiver Wuerfel", in, std::getenv("ANANAS_CUBE_JACOBI") != nullptr);
    CHECK(p.mg.converged);
}
