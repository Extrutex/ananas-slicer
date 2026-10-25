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
#include <set>
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

// Die Fälle der Regressionstests: regelmäßig, ungerade, dünn, anisotrop, kleine Lager, mehrere Teile, leere Grobzellen.
std::vector<std::pair<const char*, StressInput>> regression_cases()
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
    return cases;
}

} // namespace

TEST_CASE("Stress: multigrid gives the Jacobi result on regular, odd, thin and anisotropic bodies", "[Engine][Stress][Multigrid]")
{
    for (const auto& [name, in] : regression_cases()) {
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
    // ANANAS_BODY=LxBxH (mm) und ANANAS_VOXEL (mm) rechnen stattdessen einen Stab oder eine Platte, an der Stirnfläche x = 0
    // eingespannt und an x = L belastet (Speichermessung der Hierarchie bei Stäben und Platten).
    StressInput in = base(1.);
    if (const char* body = std::getenv("ANANAS_BODY")) {
        double l = 0., w = 0., h = 0.;
        std::sscanf(body, "%lfx%lfx%lf", &l, &w, &h);
        in.voxel_size = std::getenv("ANANAS_VOXEL") ? std::atof(std::getenv("ANANAS_VOXEL")) : 1.;
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(l, w, h)));
        clamp(in, Vec3d::Zero(), Vec3d(0, w, h));
        load(in, Vec3d(l, 0, 0), Vec3d(l, w, h), Vec3d(0, 0, -50.));
    } else {
        const double a = std::getenv("ANANAS_CUBE") ? std::atof(std::getenv("ANANAS_CUBE")) : 100.;
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(a, a, a)));
        clamp(in, Vec3d::Zero(), Vec3d(a, a, 0));
        load(in, Vec3d(0, 0, a), Vec3d(a, a, a), Vec3d(30000., 0, -30000.));
    }
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

namespace {

// Lager, die nur einzelne Achsen oder wenige Punkte halten: Rollenlager auf drei Flächen, drei Punktlager dicht beieinander,
// ein Flächenlager mit einem Punktlager für die übrige Achse.
std::vector<std::pair<const char*, StressInput>> bearing_cases()
{
    std::vector<std::pair<const char*, StressInput>> cases;
    const auto support = [](StressInput& in, const Vec3d& lo, const Vec3d& hi, std::array<bool, 3> axes) {
        StressSupport sp;
        sp.where = box(lo, hi);
        sp.axes  = axes;
        in.supports.push_back(sp);
    };
    {
        StressInput in = base(1.); // drei Rollenlager: jede Fläche hält nur eine Achse
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(30., 10., 10.)));
        support(in, Vec3d(0, 0, 0), Vec3d(0, 10, 10), { true, false, false });
        support(in, Vec3d(0, 0, 0), Vec3d(30, 0, 10), { false, true, false });
        support(in, Vec3d(0, 0, 0), Vec3d(30, 10, 0), { false, false, true });
        load(in, Vec3d(30, 0, 0), Vec3d(30, 10, 10), Vec3d(-10., 5., 3.));
        cases.emplace_back("drei Rollenlager (je eine Achse)", in);
    }
    {
        StressInput in = base(1.); // drei Punktlager auf benachbarten Knoten (3-2-1), 30 mm langes Teil
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(30., 6., 6.)));
        support(in, Vec3d(0, 0, 0), Vec3d(0, 0, 0), { true, true, true });
        support(in, Vec3d(1, 0, 0), Vec3d(1, 0, 0), { false, true, true });
        support(in, Vec3d(0, 1, 0), Vec3d(0, 1, 0), { false, false, true });
        load(in, Vec3d(30, 0, 0), Vec3d(30, 6, 6), Vec3d(0, 0, -2.));
        cases.emplace_back("drei Punktlager auf benachbarten Knoten", in);
    }
    {
        StressInput in = base(1.); // Rollenlager an der Stirnfläche (nur x), ein Punktlager für y/z und eins für z
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(41., 9., 7.)));
        support(in, Vec3d(0, 0, 0), Vec3d(0, 9, 7), { true, false, false });
        support(in, Vec3d(41, 0, 0), Vec3d(41, 0, 0), { false, true, true });
        support(in, Vec3d(41, 9, 0), Vec3d(41, 9, 0), { false, false, true });
        load(in, Vec3d(15, 0, 7), Vec3d(25, 9, 7), Vec3d(0, 0, -20.));
        cases.emplace_back("Rollenlager mit zwei Punktlagern am anderen Ende", in);
    }
    return cases;
}

double relative_difference(const StressResult& a, const StressResult& b)
{
    double d = 0., m = 0.;
    for (size_t i = 0; i < a.displacement.size(); ++i) {
        d = std::max(d, std::abs(double(a.displacement[i]) - double(b.displacement[i])));
        m = std::max(m, double(b.displacement[i]));
    }
    return d / m;
}

} // namespace

TEST_CASE("Stress: the multigrid cycle is a symmetric positive operator and the restriction is the transpose of the prolongation", "[Engine][Stress][Multigrid]")
{
    auto cases = regression_cases();
    for (auto& c : bearing_cases())
        cases.push_back(c);
    {
        // Nur die gröbste Ebene: das Gitter ist so klein, dass der dichte Löser den ganzen Körper löst.
        cases.emplace_back("nur die gröbste Ebene (10x3x3)", cantilever(10., 3., 3., 1.));
    }
    for (const auto& [name, in] : cases) {
        DYNAMIC_SECTION(name)
        {
            const StressMultigridProbe p = probe_multigrid_for_test(in);
            std::printf("[Multigrid] Operator %-46s %zu Ebenen | min <Mx,x>/<x,x> %.2e | Asymmetrie %.1e | R gegen P^T %.1e | kleinster/größter Pivot %.1e\n", name, p.levels,
                        p.min_rayleigh, p.symmetry_defect, p.transfer_defect, p.coarse_pivot_ratio);
            REQUIRE(p.built);
            CHECK(p.levels >= 1);
            CHECK(p.min_rayleigh > 0.);
            CHECK(p.symmetry_defect < 1e-9);
            CHECK(p.transfer_defect < 1e-12);
            CHECK(p.coarse_pivot_ratio > 1e-10);
            if (std::string(name).find("gröbste Ebene") != std::string::npos)
                CHECK(p.levels == 1);
        }
    }
}

TEST_CASE("Stress: bearings that hold single axes or sit close together give the Jacobi result or a clean fall back", "[Engine][Stress][Multigrid]")
{
    for (const auto& [name, in] : bearing_cases()) {
        DYNAMIC_SECTION(name)
        {
            const Pair p = both(name, in);
            REQUIRE(p.jacobi.converged);
            REQUIRE(p.mg.converged);
            if (p.mg.preconditioner == StressPreconditioner::Multigrid) {
                CHECK_FALSE(has_warning(p.mg, "multigrid"));
            } else {
                CHECK(has_warning(p.mg, "multigrid")); // sauberer Rückfall mit Warnung, nie ein falsches Ergebnis
            }
            CHECK(p.displacement_diff < 1e-4);
            CHECK(p.stress_diff < 1e-4);
            CHECK(p.max_stress_diff < 1e-4);
            CHECK(p.mg.iterations < 150);
        }
    }
    SECTION("a singular matrix of the coarsest level is refused with a warning")
    {
        StressInput in = cantilever(40., 8., 8., 1.);
        in.fault_for_test = 8;
        StressInput jac = in;
        jac.fault_for_test = 0;
        jac.preconditioner = StressPreconditioner::Jacobi;
        const StressResult ref = solve_stress(jac);
        const StressResult r   = solve_stress(in);
        REQUIRE(r.converged);
        CHECK(r.preconditioner == StressPreconditioner::Jacobi);
        CHECK(has_warning(r, "singular"));
        CHECK(relative_difference(r, ref) < 1e-9);
    }
}

TEST_CASE("Stress: a spectrum estimate that is too small is caught by the check of the cycle", "[Engine][Stress][Multigrid]")
{
    StressInput jac = cantilever(40., 8., 8., 1.);
    jac.preconditioner = StressPreconditioner::Jacobi;
    const StressResult ref = solve_stress(jac);
    REQUIRE(ref.converged);

    StressInput in = cantilever(40., 8., 8., 1.);
    SECTION("the estimate is right")
    {
        const StressMultigridProbe p = probe_multigrid_for_test(in);
        CHECK(p.built);
        const StressResult r = solve_stress(in);
        CHECK(r.preconditioner == StressPreconditioner::Multigrid);
        CHECK_FALSE(has_warning(r, "multigrid"));
    }
    SECTION("the estimate is far too small")
    {
        in.fault_for_test = 4;
        const StressMultigridProbe p = probe_multigrid_for_test(in);
        CHECK_FALSE(p.built);
        CHECK(p.why.find("positive definite") != std::string::npos);
        const StressResult r = solve_stress(in);
        REQUIRE(r.converged);
        CHECK(r.preconditioner == StressPreconditioner::Jacobi);
        CHECK(has_warning(r, "positive definite"));
        CHECK(r.cg_restarts == 0); // gefangen vor dem ersten Schritt, nicht erst mitten im Lauf
        CHECK(r.iterations == ref.iterations);
        CHECK(relative_difference(r, ref) < 1e-9);
    }
}

TEST_CASE("Stress: a fall back to Jacobi in the middle of the run restarts the conjugate gradient", "[Engine][Stress][Multigrid]")
{
    // Der Rückfall mitten im Lauf (ein Zyklus liefert nichts Brauchbares) startet das CG neu: neue Suchrichtung p = z, rz = <r,z>.
    // Ohne Neustart würde beta = rz_Jacobi / rz_Multigrid aus Skalaren zweier Vorkonditionierer gebildet und die alte
    // Richtung weitergeführt.
    std::vector<std::pair<const char*, StressInput>> cases;
    cases.emplace_back("Platte 60x40x1", cantilever(60., 40., 1., 1.));
    cases.emplace_back("Hohlkasten 16, Wand 1 Zelle", regression_cases()[10].second);
    cases.emplace_back("Kragarm 96x6x6, Voxel 0,5", cantilever(96., 6., 6., 0.5));
    for (const auto& [name, in] : cases) {
        DYNAMIC_SECTION(name)
        {
            StressInput jac = in;
            jac.preconditioner = StressPreconditioner::Jacobi;
            const StressResult ref = solve_stress(jac);
            const StressResult clean = solve_stress(in);
            REQUIRE(ref.converged);
            REQUIRE(clean.converged);
            for (int fault : { 2, 110 }) {
                StressInput bad = in;
                bad.fault_for_test = fault;
                const StressResult r = solve_stress(bad);
                std::printf("[Multigrid] Rückfall %-30s Fehler %3d: %5zu Iterationen (Multigrid allein %3zu, Jacobi allein %5zu), Neustarts %zu\n", name, fault, r.iterations,
                            clean.iterations, ref.iterations, r.cg_restarts);
                REQUIRE(r.converged);
                CHECK(r.preconditioner == StressPreconditioner::Jacobi);
                CHECK(has_warning(r, "multigrid"));
                CHECK(r.cg_restarts == (clean.iterations > size_t(fault >= 100 ? fault - 100 : 3) ? 1u : 0u));
                CHECK(relative_difference(r, ref) < 1e-4);
                CHECK(r.max_von_mises == Catch::Approx(ref.max_von_mises).epsilon(1e-4));
                // Gemessen (Fehler 2 / 110): mit Neustart 1760 / 1767 (Platte), 1334 / 940 (Balken), 322 / 291 (Hohlkasten); die alte
                // Fortführung der Richtung mit beta = rz_Jacobi / rz_Multigrid brauchte 5060 / 5035, 6116 / 948, 712 / 345.
                // Jacobi startet aus einem Multigrid-Zwischenstand langsamer als aus null (das Restfeld ist glatt), daher die Grenze
                // bei dem Doppelten.
                CHECK(r.iterations <= 2 * ref.iterations + 60);
            }
        }
    }
}

TEST_CASE("Stress: the clock is read in every phase of the multigrid setup and cycle", "[Engine][Stress][Multigrid]")
{
    const StressInput good = cantilever(96., 6., 6., 0.5);
    const auto        message = [](StressInput in) {
        try {
            solve_stress(in);
        } catch (const EngineError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    SECTION("the clock runs out before the spectrum estimate")
    {
        StressInput in = good;
        in.fault_for_test = 5;
        const std::string m = message(in);
        CHECK(m.find("time limit") != std::string::npos);
        CHECK(m.find("multigrid") != std::string::npos);
    }
    SECTION("the clock runs out in the first cycle of the solver, between the coarse correction and the post-smoothing")
    {
        StressInput in = good;
        in.fault_for_test = 6;
        const std::string m = message(in);
        CHECK(m.find("time limit") != std::string::npos);
        CHECK(m.find("preparing the solver") != std::string::npos);
    }
    SECTION("the clock runs out in the third cycle of the solver")
    {
        StressInput in = good;
        in.fault_for_test = 7;
        const std::string m = message(in);
        CHECK(m.find("time limit") != std::string::npos);
        INFO(m);
        CHECK(m.find("while solving") != std::string::npos);
    }
    SECTION("every limit between a millisecond and the run time either gives a time-limit error or a converged result")
    {
        // Wanderndes Limit über Voxelisierung, Aufbau, Spektrumschätzung, Zyklen und Nachglättung: nie ein anderer Fehler und nie
        // ein nicht konvergiertes Ergebnis.
        std::set<std::string> phases;
        size_t converged = 0, timeouts = 0;
        for (double seconds = 0.0005; seconds < 0.5; seconds *= 1.25) {
            StressInput in = good;
            in.max_seconds = seconds;
            try {
                const StressResult r = solve_stress(in);
                CHECK(r.converged);
                ++converged;
            } catch (const EngineError& e) {
                const std::string m = e.what();
                REQUIRE(m.find("time limit") != std::string::npos);
                const size_t at = m.find("while ");
                phases.insert(m.substr(at, m.find(';', at) - at));
                ++timeouts;
            }
        }
        std::printf("[Multigrid] Zeitlimit-Durchlauf: %zu Abbrüche in %zu Phasen, %zu konvergierte Ergebnisse\n", timeouts, phases.size(), converged);
        for (const std::string& ph : phases)
            std::printf("[Multigrid]   Abbruch %s\n", ph.c_str());
        CHECK(timeouts > 0);
        CHECK(converged > 0);
    }
}

TEST_CASE("Stress: the memory estimate of the hierarchy follows the shape of the body", "[Engine][Stress][Multigrid]")
{
    // Die Hierarchie kostet pro Feinknoten sehr unterschiedlich viel: kompakte Körper verlieren je Ebene den Faktor 8, Platten 4,
    // Stäbe 2. Die Schätzung vor dem Aufbau muss darüber liegen (sonst läuft das Speicherbudget unbemerkt über) und darf nicht
    // weit darüber liegen (sonst fällt der Löser ohne Not auf Jacobi zurück).
    std::vector<std::pair<const char*, StressInput>> cases;
    cases.emplace_back("Würfel 48 mm, 1 mm", [] {
        StressInput in = base(1.);
        in.parts.push_back(box_mesh(Vec3d::Zero(), Vec3d(48., 48., 48.)));
        clamp(in, Vec3d::Zero(), Vec3d(48, 48, 0));
        load(in, Vec3d(0, 0, 48), Vec3d(48, 48, 48), Vec3d(300., 0, -300.));
        return in;
    }());
    cases.emplace_back("Platte 120x120x2, 0,5 mm", cantilever(120., 120., 2., 0.5));
    cases.emplace_back("Stab 200x4x4, 0,25 mm", cantilever(200., 4., 4., 0.25));
    cases.emplace_back("Stab 300x6x6, 0,5 mm", cantilever(300., 6., 6., 0.5));
    cases.emplace_back("Stab 400x1x1, 1 mm", cantilever(400., 1., 1., 1.)); // eine Zelle Querschnitt: der teuerste Fall je Knoten
    for (const auto& [name, in] : cases) {
        DYNAMIC_SECTION(name)
        {
            const StressMultigridProbe p = probe_multigrid_for_test(in);
            REQUIRE(p.built);
            const StressResult r = solve_stress(in);
            std::printf("[Multigrid] Speicher %-26s %8zu Knoten, %2zu Ebenen: Hierarchie %7.1f MB (%6.1f B je Feinknoten), Schätzung %7.1f MB\n", name, r.nodes, p.levels,
                        double(p.hierarchy_bytes) / 1048576., double(p.hierarchy_bytes) / double(r.nodes), double(p.estimated_bytes) / 1048576.);
            CHECK(p.estimated_bytes >= p.hierarchy_bytes);
            CHECK(p.estimated_bytes <= 3 * p.hierarchy_bytes);
        }
    }
}
