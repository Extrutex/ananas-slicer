// Validierung des linear-elastischen Lösers (analyze.stress) gegen geschlossene Referenzformeln an Fällen, die
// anspruchsvoller sind als ein Zugstab: Platte mit Bohrung, Dreipunktbiegung, dickwandiges Rohr unter Innendruck,
// Anisotropie quer zu den Schichten und ein Winkelhalter. Die Zahlen sind Richtwerte, nicht zertifiziert. Die
// gemessenen Fehler und ihre Deutung stehen in docs/HLSD/stress-validierung.md. Die Toleranzen sind aus den gemessenen
// Fehlern abgeleitet (mit knapper Reserve), nicht aus der Wunschvorstellung.
//
// Die feinsten Stufen (viele Zellen, Minuten Rechenzeit) laufen nur, wenn ANANAS_STRESS_FULL gesetzt ist.
#include <catch2/catch_all.hpp>

#include "engine/Stress.hpp"

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Triangulation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::engine;

namespace {

constexpr double PI_ = 3.14159265358979323846;

StressSelection box(const Vec3d& lo, const Vec3d& hi)
{
    StressSelection s;
    s.box = BoundingBoxf3(lo, hi);
    return s;
}

StressInput base(const indexed_triangle_set& mesh, double voxel, double E = 2000., double nu = 0.3)
{
    StressInput in;
    in.parts.push_back(mesh);
    in.voxel_size              = voxel;
    in.material.youngs_modulus = E;
    in.material.poisson_ratio  = nu;
    in.material.yield_strength = 40.;
    return in;
}

Polygon polygon_of(const std::vector<Vec2d>& pts)
{
    Polygon p;
    for (const Vec2d& v : pts)
        p.points.emplace_back(scaled(v.x()), scaled(v.y()));
    return p;
}

std::vector<Vec2d> circle(double cx, double cy, double r, int n, bool ccw)
{
    std::vector<Vec2d> pts;
    for (int i = 0; i < n; ++i) {
        const double a = 2. * PI_ * i / n * (ccw ? 1. : -1.);
        pts.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
    }
    return pts;
}

// Prisma der Höhe `height` (z) über einer Kontur mit Löchern als geschlossenes Netz. `wall_begin[i]` ist der Index des
// ersten Wanddreiecks der Schleife i (0 = Kontur, danach die Löcher), je Kante zwei Dreiecke in Kantenreihenfolge.
struct Prism
{
    indexed_triangle_set its;
    std::vector<size_t>  wall_begin;
};

Prism extrude(const ExPolygon& ex, double height)
{
    Prism       out;
    const Points pts = to_points(ex);
    const auto   tri = Triangulation::triangulate(ex);
    const int    n   = int(pts.size());
    for (const Point& p : pts)
        out.its.vertices.emplace_back(float(unscaled(p.x())), float(unscaled(p.y())), 0.f);
    for (const Point& p : pts)
        out.its.vertices.emplace_back(float(unscaled(p.x())), float(unscaled(p.y())), float(height));
    for (const Vec3i32& t : tri) {
        const Vec2d a = unscaled(pts[t[0]]).cast<double>(), b = unscaled(pts[t[1]]).cast<double>(), c = unscaled(pts[t[2]]).cast<double>();
        const double area2 = (b - a).x() * (c - a).y() - (b - a).y() * (c - a).x();
        Vec3i32      u = t;
        if (area2 < 0) std::swap(u[1], u[2]); // gegen den Uhrzeigersinn von +z gesehen
        out.its.indices.emplace_back(u[0] + n, u[1] + n, u[2] + n); // oben: Normale +z
        out.its.indices.emplace_back(u[0], u[2], u[1]);             // unten: Normale -z
    }
    int base_index = 0;
    auto wall = [&](const Polygon& loop) {
        out.wall_begin.push_back(out.its.indices.size());
        const int m = int(loop.points.size());
        for (int i = 0; i < m; ++i) {
            const int a = base_index + i, b = base_index + (i + 1) % m;
            out.its.indices.emplace_back(a, b, b + n);
            out.its.indices.emplace_back(a, b + n, a + n);
        }
        base_index += m;
    };
    wall(ex.contour);
    for (const Polygon& h : ex.holes)
        wall(h);
    if (its_volume(out.its) < 0.f)
        its_flip_triangles(out.its);
    return out;
}

size_t cell_index(const StressResult& r, const Vec3d& p)
{
    const Vec3d g = (p - r.origin) / r.voxel_size;
    const auto  c = [&](int a) { return size_t(std::clamp(int(std::floor(g[a])), 0, int(r.size[a]) - 1)); };
    return c(0) + r.size[0] * (c(1) + r.size[1] * c(2));
}

double rel_err(double value, double ref) { return (value - ref) / ref; }

bool full_run() { return std::getenv("ANANAS_STRESS_FULL") != nullptr; }

void note_warnings(const StressResult& r)
{
    for (const auto& w : r.warnings)
        std::printf("   Hinweis des Lösers: %s\n", w.c_str());
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// (a) Platte mit Mittelbohrung unter Zug (Breite W, Bohrung d, Dicke t). Referenz: Peterson, "Stress Concentration
// Factors" (Pilkey), Diagramm 4.1 (endliche Breite, Howland/Heywood):
//   Ktn = 3,00 - 3,140 (d/W) + 3,667 (d/W)^2 - 1,527 (d/W)^3 (andere Fassung: 3,13 / 3,66 / 1,53, Unterschied 0,08 %), bezogen auf die Nettoquerschnittsspannung F / ((W - d) t);
//   bezogen auf die Bruttospannung F / (W t): Ktg = Ktn / (1 - d/W). Die Vergleichsspannung am Bohrungsrand ist die
// Tangentialspannung (sigma_r = 0 am freien Rand, ebener Spannungszustand).
namespace {

struct PlateRun
{
    double voxel, max_vm, kt, error;
    size_t iterations;
    bool   converged;
    double reaction, applied;
};

PlateRun run_plate(double voxel, double W, double L, double t, double d, double F)
{
    ExPolygon ex(polygon_of({ { 0, 0 }, { L, 0 }, { L, W }, { 0, W } }));
    ex.holes.push_back(polygon_of(circle(L / 2, W / 2, d / 2, 240, false)));
    ex.contour.make_counter_clockwise();
    ex.holes[0].make_clockwise();
    StressInput in = base(extrude(ex, t).its, voxel);
    StressSupport face; // Einspannseite: darf nur in ihrer Ebene gleiten
    face.where = box(Vec3d::Zero(), Vec3d(0, W, t));
    face.axes  = { true, false, false };
    in.supports.push_back(face);
    StressSupport corner;
    corner.where = box(Vec3d::Zero(), Vec3d::Zero());
    corner.axes  = { false, true, true };
    in.supports.push_back(corner);
    StressSupport along;
    along.where = box(Vec3d(0, W, 0), Vec3d(0, W, 0));
    along.axes  = { false, false, true };
    in.supports.push_back(along);
    StressLoad load;
    load.where = box(Vec3d(L, 0, 0), Vec3d(L, W, t));
    load.force = Vec3d(F, 0, 0);
    in.loads.push_back(load);
    const StressResult r = solve_stress(in);
    const double sigma = F / (W * t), dw = d / W;
    const double ktn = 3.00 - 3.140 * dw + 3.667 * dw * dw - 1.527 * dw * dw * dw, ktg = ktn / (1. - dw);
    PlateRun out{ voxel, r.max_von_mises, r.max_von_mises / sigma, rel_err(r.max_von_mises / sigma, ktg), r.iterations, r.converged,
                  r.reaction.x(), r.applied_force.x() };
    std::printf("[Validierung a] Platte mit Bohrung, Voxel %.2f (%.0f Zellen je Bohrungsradius, %zu Zellen): Kt(brutto) %.4f, "
                "Referenz %.4f (Ktn %.4f), Fehler %+.1f %%, %zu Iterationen, Residuum %.1e\n",
                voxel, d / 2 / voxel, r.solid_cells, out.kt, ktg, ktn, 100. * out.error, r.iterations, r.residual);
    note_warnings(r);
    return out;
}

} // namespace

TEST_CASE("Stress-Validierung: Platte mit Bohrung gegen Peterson-Kt, Verfeinerungsreihe", "[Engine][Stress][Validation]")
{
    const double W = 24., L = 48., t = 2., d = 6., F = 480.; // Bruttospannung 10 MPa, d/W = 0,25
    std::vector<PlateRun> runs;
    for (double voxel : { 0.5, 0.25 })
        runs.push_back(run_plate(voxel, W, L, t, d, F));
    if (full_run())
        runs.push_back(run_plate(0.2, W, L, t, d, F));
    for (const PlateRun& r : runs) {
        REQUIRE(r.converged);
        CHECK(r.reaction == Catch::Approx(-F).epsilon(1e-4));
        CHECK(r.applied == Catch::Approx(F).epsilon(1e-9));
        CHECK(r.error < 0.); // Voxelnetz mit Elementmittelwert unterschätzt die Kerbspannung
    }
    // Gemessen: -18,0 % (0,5 mm), -6,3 % (0,25 mm), -3,3 % (0,2 mm). Der Fehler muss mit der Verfeinerung schrumpfen.
    CHECK(runs[0].error > -0.22);
    CHECK(runs[0].error < -0.14);
    CHECK(runs[1].error > -0.09);
    CHECK(runs[1].error < -0.03);
    CHECK(std::abs(runs[1].error) < std::abs(runs[0].error));
    if (runs.size() > 2) {
        CHECK(runs[2].error > -0.06);
        CHECK(runs[2].error < -0.015);
        CHECK(std::abs(runs[2].error) < std::abs(runs[1].error));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// (b) Dreipunktbiegung eines Rechteckbalkens (Stützweite L, Querschnitt w x h), Linienlager unten an beiden Enden,
// Last in der Mitte oben auf einem 2 mm breiten Streifen. Referenz Timoshenko: F L^3 / (48 E I) + F L / (4 kappa G A)
// mit kappa nach Cowper = 10 (1 + nu) / (12 + 11 nu); Randfaserspannung M c / I mit M = F L / 4 im Zellmittelpunkt der
// untersten Zellschicht (Abstand c = h/2 - Voxel/2 von der neutralen Faser).
namespace {

struct Bend
{
    double L = 48., w = 6., h = 6., F = 100., E = 2000., nu = 0.3;
    double kappa() const { return 10. * (1. + nu) / (12. + 11. * nu); }
    double I() const { return w * h * h * h / 12.; }
    double timoshenko() const
    {
        return F * L * L * L / (48. * E * I()) + F * L / (4. * kappa() * E / (2. * (1. + nu)) * w * h);
    }
    double sigma_at(double c) const { return F * L / 4. * c / I(); }
};

StressInput three_point(const Bend& b, double voxel)
{
    StressInput in = base(its_make_cube(b.L, b.w, b.h), voxel, b.E, b.nu);
    StressSupport left;
    left.where = box(Vec3d(0, 0, 0), Vec3d(0, b.w, 0));
    left.axes  = { false, false, true };
    in.supports.push_back(left);
    StressSupport pin; // hält x und y in einer Ecke
    pin.where = box(Vec3d(0, 0, 0), Vec3d(0, 0, 0));
    pin.axes  = { true, true, false };
    in.supports.push_back(pin);
    StressSupport pin2; // hält die Drehung um z: x in der anderen Ecke desselben Lagers
    pin2.where = box(Vec3d(0, b.w, 0), Vec3d(0, b.w, 0));
    pin2.axes  = { true, false, false };
    in.supports.push_back(pin2);
    StressSupport right;
    right.where = box(Vec3d(b.L, 0, 0), Vec3d(b.L, b.w, 0));
    right.axes  = { false, false, true };
    in.supports.push_back(right);
    StressLoad load;
    load.where = box(Vec3d(b.L / 2 - 1., 0, b.h), Vec3d(b.L / 2 + 1., b.w, b.h));
    load.force = Vec3d(0, 0, -b.F);
    in.loads.push_back(load);
    return in;
}

struct BendRun { double deflection_error, stress_error; };

BendRun run_bend(const Bend& b, double voxel)
{
    const StressResult r = solve_stress(three_point(b, voxel));
    REQUIRE(r.converged);
    CHECK(r.reaction.z() == Catch::Approx(b.F).epsilon(1e-4));
    const double ref   = b.timoshenko();
    const double sig   = b.sigma_at(b.h / 2 - voxel / 2);
    const double vm    = r.von_mises[cell_index(r, Vec3d(b.L / 2, b.w / 2, voxel / 2))];
    const BendRun out{ rel_err(r.max_displacement, ref), rel_err(vm, sig) };
    std::printf("[Validierung b] Dreipunktbiegung L/h %.0f, Voxel %.2f (%.0f Zellen über die Höhe): Durchbiegung %.4f mm, Timoshenko %.4f mm, "
                "Fehler %+.1f %%; Randfaserspannung Feldmitte %.2f MPa, Balkentheorie %.2f MPa, Fehler %+.1f %%; "
                "globales Maximum %.1f MPa bei x = %.1f (Last- bzw. Lagerkante, kein Feldwert); %zu Iterationen\n",
                b.L / b.h, voxel, b.h / voxel, r.max_displacement, ref, 100. * out.deflection_error, vm, sig, 100. * out.stress_error,
                r.max_von_mises, r.max_von_mises_at.x(), r.iterations);
    note_warnings(r);
    return out;
}

} // namespace

TEST_CASE("Stress-Validierung: Dreipunktbiegung gegen Timoshenko, Verfeinerungsreihe", "[Engine][Stress][Validation]")
{
    // Schlanke Stufe zuerst: L/h = 16. Gemessen Durchbiegung: -0,9 % (1,0), +0,3 % (0,5), +0,4 % (0,4), +0,6 % (0,3);
    // Randfaserspannung -3,6 % (1,0), -2,2 % (0,5), -2,1 % (0,4), -1,8 % (0,3).
    Bend slender;
    slender.L = 96.;
    slender.F = 50.;
    std::vector<BendRun> s;
    for (double voxel : { 1., 0.5 })
        s.push_back(run_bend(slender, voxel));
    if (full_run())
        s.push_back(run_bend(slender, 0.3));
    for (const BendRun& r : s) {
        CHECK(std::abs(r.deflection_error) < 0.02);
        CHECK(r.stress_error < 0.);
        CHECK(r.stress_error > -0.06);
    }
    CHECK(std::abs(s[1].stress_error) < std::abs(s[0].stress_error)); // Spannung nähert sich beim Verfeinern der Theorie

    // Gedrungene Stufe L/h = 8. Gemessen Durchbiegung: +1,4 % (1,0), +3,2 % (0,5), +4,1 % (0,25), also WACHSEND und
    // oberhalb der Balkentheorie: die Balkentheorie enthält weder die örtliche Eindrückung unter Last und Lagern noch
    // die Querkontraktion, der Löser (dreidimensional) schon. Randfaserspannung -5,8 %, -4,0 %, -3,4 %.
    Bend stocky;
    std::vector<BendRun> g;
    for (double voxel : { 1., 0.5 })
        g.push_back(run_bend(stocky, voxel));
    if (full_run())
        g.push_back(run_bend(stocky, 0.25));
    for (const BendRun& r : g) {
        CHECK(r.deflection_error > 0.);
        CHECK(r.deflection_error < 0.06);
        CHECK(r.stress_error < 0.);
        CHECK(r.stress_error > -0.08);
    }
    CHECK(std::abs(g[1].stress_error) < std::abs(g[0].stress_error));
}

// ---------------------------------------------------------------------------------------------------------------------
// (c) Dickwandiges Rohr (Innenradius a, Außenradius b) unter Innendruck p, freie Enden. Referenz Lamé im ebenen
// Spannungszustand (sigma_z = 0, für Druck nur auf der Bohrung exakt):
//   u_r(r) = p a^2 / (b^2 - a^2) * ((1 - nu) r + (1 + nu) b^2 / r) / E,
//   sigma_theta(r) = p a^2 / (b^2 - a^2) (1 + b^2 / r^2),  sigma_r(r) = p a^2 / (b^2 - a^2) (1 - b^2 / r^2).
// Der Druck wird als 32 Teillasten auf die Bohrungsdreiecke gelegt (Kraft = -p * Summe der Flächenvektoren, also exakt
// die Druckresultierende je Sektor). Gelagert wird nur dort, wo die rotationssymmetrische Lösung keine Verschiebung hat
// (Mittelhöhe, Symmetrieachsen), damit die Lager die Lösung nicht stören.
namespace {

struct TubeRun
{
    double axis_error, mean_vm_error[3], max_disp_error, max_vm_error;
    bool   converged;
    double force_residual;
};

TubeRun run_tube(double voxel)
{
    const double a = 10., bo = 20., Lz = 8., p = 5., E = 2000., nu = 0.3, cx = 20., cy = 20.;
    const int    n_in = 96, n_out = 192, sectors = 32;
    ExPolygon ex(polygon_of(circle(cx, cy, bo, n_out, true)));
    ex.holes.push_back(polygon_of(circle(cx, cy, a, n_in, false)));
    ex.contour.make_counter_clockwise();
    ex.holes[0].make_clockwise();
    const Prism   pr = extrude(ex, Lz);
    StressInput   in = base(pr.its, voxel, E, nu);
    const size_t  per = size_t(n_in / sectors);
    for (int s = 0; s < sectors; ++s) {
        StressLoad load;
        Vec3d      sum = Vec3d::Zero();
        for (size_t e = 0; e < per * 2; ++e) {
            const size_t ti = pr.wall_begin[1] + size_t(s) * per * 2 + e;
            load.where.triangles.push_back(ti);
            const auto& t = pr.its.indices[ti];
            const Vec3d v0 = pr.its.vertices[t[0]].cast<double>(), v1 = pr.its.vertices[t[1]].cast<double>(), v2 = pr.its.vertices[t[2]].cast<double>();
            sum += 0.5 * (v1 - v0).cross(v2 - v0); // nach außen aus dem Körper, also in die Bohrung
        }
        load.force = -p * sum;
        in.loads.push_back(load);
    }
    const double zc = Lz / 2;
    auto hold = [&](double dx, double dy, std::array<bool, 3> axes) {
        const double sx = dx > 0 ? 1. : dx < 0 ? -1. : 0., sy = dy > 0 ? 1. : dy < 0 ? -1. : 0.;
        const Vec3d  c(cx + dx, cy + dy, zc), in_ward(-sx * 1.1 * voxel, -sy * 1.1 * voxel, 0);
        const Vec3d  pad(0.1 * voxel, 0.1 * voxel, 0.1 * voxel);
        StressSupport s;
        s.where = box(c.cwiseMin(c + in_ward) - pad, c.cwiseMax(c + in_ward) + pad);
        s.axes  = axes;
        in.supports.push_back(s);
    };
    hold(bo, 0, { false, true, true });
    hold(-bo, 0, { false, true, true });
    hold(0, bo, { true, false, true });
    hold(0, -bo, { true, false, true });
    const StressResult r = solve_stress(in);

    const double k = p * a * a / (bo * bo - a * a);
    auto u_at  = [&](double rr) { return k * ((1 - nu) * rr + (1 + nu) * bo * bo / rr) / E; };
    auto vm_at = [&](double rr) {
        const double th = k * (1 + bo * bo / (rr * rr)), rd = k * (1 - bo * bo / (rr * rr));
        return std::sqrt(th * th + rd * rd - th * rd);
    };
    TubeRun out{};
    out.converged = r.converged;
    const double r0 = a + 0.5 * voxel; // Mitte der innersten Zellschicht auf der x-Achse
    out.axis_error = rel_err(r.displacement_vec[3 * cell_index(r, Vec3d(cx + r0, cy, zc - 0.01))], u_at(r0));
    int idx = 0;
    for (double rr : { 12., 15., 18. }) { // Mittel über den Umfang (alle 5 Grad), Mittelebene
        double sum = 0;
        int    cnt = 0;
        for (int ang = 0; ang < 360; ang += 5, ++cnt)
            sum += r.von_mises[cell_index(r, Vec3d(cx + rr * std::cos(ang * PI_ / 180), cy + rr * std::sin(ang * PI_ / 180), zc - 0.01))];
        out.mean_vm_error[idx++] = rel_err(sum / cnt, vm_at(rr));
    }
    out.max_disp_error = rel_err(r.max_displacement, u_at(a));
    out.max_vm_error   = rel_err(r.max_von_mises, vm_at(a));
    out.force_residual = (r.applied_force).norm() + r.reaction.norm();
    std::printf("[Validierung c] Rohr a=%.0f b=%.0f p=%.0f, Voxel %.2f (%.0f Zellen über die Wandstärke, %zu Zellen): "
                "u_r nahe Bohrung auf der Achse %+.1f %%; mittlere Vergleichsspannung r=12/15/18: %+.1f / %+.1f / %+.1f %%; "
                "größte Verschiebung %.4f mm (Lamé an der Bohrung %.4f, %+.1f %%); größte Vergleichsspannung %.2f MPa "
                "(Lamé an der Bohrung %.2f, %+.1f %%); %zu Iterationen\n",
                a, bo, p, voxel, (bo - a) / voxel, r.solid_cells, 100. * out.axis_error, 100. * out.mean_vm_error[0],
                100. * out.mean_vm_error[1], 100. * out.mean_vm_error[2], r.max_displacement, u_at(a), 100. * out.max_disp_error,
                r.max_von_mises, vm_at(a), 100. * out.max_vm_error, r.iterations);
    note_warnings(r);
    return out;
}

} // namespace

TEST_CASE("Stress-Validierung: dickwandiges Rohr unter Innendruck gegen Lamé, Verfeinerungsreihe", "[Engine][Stress][Validation]")
{
    // Gemessen (1,0 / 0,5 / 0,25 mm): u_r auf der Achse +1,9 / -0,3 / +0,7 %; mittlere Vergleichsspannung r=12:
    // +3,5 / +2,0 / 0,0 %, r=15: +3,0 / +1,7 / +0,2 %, r=18: +2,3 / +1,4 / +0,2 %; größte Verschiebung (örtliche
    // Treppenzelle an der Bohrung) +10,1 / +10,4 / +3,9 %; größte Vergleichsspannung +1,9 / +7,3 / +18,0 % (wächst: Treppenkanten).
    const double tol_vm[3]   = { 0.045, 0.025, 0.006 };
    const double tol_axis[3] = { 0.03, 0.02, 0.02 };
    const double tol_disp[3] = { 0.14, 0.14, 0.06 };
    int i = 0;
    for (double voxel : { 1., 0.5, 0.25 }) {
        const TubeRun r = run_tube(voxel);
        REQUIRE(r.converged);
        CHECK(r.force_residual < 1e-3); // Druckresultierende ist null, Lagerkräfte ebenso
        CHECK(std::abs(r.axis_error) < tol_axis[i]);
        for (double e : r.mean_vm_error)
            CHECK(std::abs(e) < tol_vm[i]);
        CHECK(r.max_disp_error > 0.);
        CHECK(r.max_disp_error < tol_disp[i]);
        ++i;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// (d) Anisotropie quer zu den Schichten: dieselbe Zugprobe in x und in z gedruckt. Mittlere Dehnung aus der Verschiebung
// der ersten und der letzten Zellschicht; Verhältnis der Dehnungen = 1 / z_factor, Verhältnis der Sicherheiten = z_factor.
namespace {

StressInput tension_bar(int axis, double w, double L, double voxel, double force, double z_factor)
{
    Vec3d dim(w, w, w);
    dim[axis] = L;
    StressInput in = base(its_make_cube(dim.x(), dim.y(), dim.z()), voxel);
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
    StressSupport along_u;
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

double axial_strain(const StressResult& r, int axis)
{
    double first = 0, last = 0;
    size_t nf = 0, nl = 0;
    const size_t nx = r.size[0], ny = r.size[1], nz = r.size[2];
    for (size_t k = 0; k < nz; ++k)
        for (size_t j = 0; j < ny; ++j)
            for (size_t i = 0; i < nx; ++i) {
                const size_t idx = i + nx * (j + ny * k);
                if (!r.solid[idx]) continue;
                const size_t pos = axis == 0 ? i : axis == 1 ? j : k;
                if (pos == 0) { first += r.displacement_vec[3 * idx + axis]; ++nf; }
                if (pos == r.size[axis] - 1) { last += r.displacement_vec[3 * idx + axis]; ++nl; }
            }
    return (last / double(nl) - first / double(nf)) / (double(r.size[axis] - 1) * r.voxel_size);
}

} // namespace

TEST_CASE("Stress-Validierung: Zugprobe in x und in z gedruckt, Verhältnis = z_factor", "[Engine][Stress][Validation]")
{
    const double w = 4., L = 20., F = 100., E = 2000., sigma = F / (w * w);
    for (double voxel : { 1., 0.5 })
        for (double zf : { 0.3, 0.5, 0.8 }) {
            const StressResult rx = solve_stress(tension_bar(0, w, L, voxel, F, zf));
            const StressResult rz = solve_stress(tension_bar(2, w, L, voxel, F, zf));
            REQUIRE(rx.converged);
            REQUIRE(rz.converged);
            const double ex = axial_strain(rx, 0), ez = axial_strain(rz, 2);
            std::printf("[Validierung d] z_factor %.2f, Voxel %.2f: Dehnung x %.6e (sigma/E %.6e), Dehnung z %.6e, Verhältnis %.5f "
                        "(1/z_factor %.5f), Sicherheit x %.3f, z %.3f, Verhältnis %.5f; größte Verschiebung z/x %.5f "
                        "(enthält die Querkontraktion, deshalb etwas kleiner)\n",
                        zf, voxel, ex, sigma / E, ez, ez / ex, 1. / zf, rx.min_safety, rz.min_safety, rz.min_safety / rx.min_safety,
                        rz.max_displacement / rx.max_displacement);
            // Gemessen: Abweichung 0,000 % (Zugstab liegt im Elementraum), nur die Löser-Toleranz bleibt.
            CHECK(ex == Catch::Approx(sigma / E).epsilon(1e-4));
            CHECK(ez / ex == Catch::Approx(1. / zf).epsilon(1e-4));
            CHECK(rz.min_safety / rx.min_safety == Catch::Approx(zf).epsilon(1e-4));
        }
}

// ---------------------------------------------------------------------------------------------------------------------
// (e) Winkelhalter (L-Profil mit Bohrung im senkrechten Schenkel), Rückwand x = 0 fest, Last am Ende des waagerechten
// Arms. Balkenabschätzung: Biegespannung 6 F l / (b t^2) mit l = Lx - t (Arm ab der inneren Ecke); Durchbiegung am
// Ende zwischen Kragarm der Länge Lx - t (starre Ecke, untere Schranke) und Lx (Einspannung an der Wand, obere
// Schranke), jeweils mit Schubanteil. Die scharfe innere Ecke ist eine Singularität: ihre Spannung hat KEINEN
// Grenzwert und wächst mit der Verfeinerung; verglichen wird deshalb nur die Größenordnung.
namespace {

struct BracketRun
{
    double voxel, max_vm, safety, tip, arm_error_top, arm_error_bottom, corner_distance;
    bool   converged;
};

BracketRun run_bracket(double voxel, double beam_sigma_corner, double tip_low, double tip_high)
{
    const double Lx = 50., Ly = 40., t = 8., bw = 10., F = 50., E = 2000., nu = 0.3;
    ExPolygon ex(polygon_of({ { 0, 0 }, { Lx, 0 }, { Lx, t }, { t, t }, { t, Ly }, { 0, Ly } }));
    ex.holes.push_back(polygon_of(circle(t / 2, Ly - 12., 1.5, 48, false)));
    ex.contour.make_counter_clockwise();
    ex.holes[0].make_clockwise();
    StressInput in = base(extrude(ex, bw).its, voxel, E, nu);
    StressSupport wall;
    wall.where = box(Vec3d::Zero(), Vec3d(0, Ly, bw));
    in.supports.push_back(wall);
    StressLoad load;
    load.where = box(Vec3d(Lx, 0, 0), Vec3d(Lx, t, bw));
    load.force = Vec3d(0, -F, 0);
    in.loads.push_back(load);
    const StressResult r = solve_stress(in);
    // Nennspannung im Arm bei x = 3 t (weit von Ecke und Last) gegen die Zellmitten-Balkenspannung
    const double x = 3 * t, sb = 6. * F * (Lx - x) / (bw * t * t) * (1. - voxel / t);
    const double vt = r.von_mises[cell_index(r, Vec3d(x, t - voxel / 2, bw / 2))], vb = r.von_mises[cell_index(r, Vec3d(x, voxel / 2, bw / 2))];
    BracketRun out{ voxel, r.max_von_mises, r.min_safety, r.max_displacement, rel_err(vt, sb), rel_err(vb, sb),
                    std::max(std::abs(r.max_von_mises_at.x() - t), std::abs(r.max_von_mises_at.y() - t)), r.converged };
    std::printf("[Validierung e] Winkelhalter, Voxel %.2f (%.0f Zellen über die Armdicke, %zu Zellen): größte Vergleichsspannung %.2f MPa bei "
                "(%.2f %.2f %.2f) (innere Ecke (%.0f %.0f)), Sicherheit %.3f (Balken %.3f), Verhältnis zur Balkenspannung %.2f, "
                "Durchbiegung %.4f mm (Balken %.4f ... %.4f); Armspannung bei x=%.0f oben %+.1f %% unten %+.1f %% gegen Balken; %zu Iterationen\n",
                voxel, t / voxel, r.solid_cells, r.max_von_mises, r.max_von_mises_at.x(), r.max_von_mises_at.y(), r.max_von_mises_at.z(), t, t,
                r.min_safety, 40. / beam_sigma_corner, r.max_von_mises / beam_sigma_corner, r.max_displacement, tip_low, tip_high, x,
                100. * out.arm_error_top, 100. * out.arm_error_bottom, r.iterations);
    CHECK(r.reaction.y() == Catch::Approx(F).epsilon(1e-4));
    note_warnings(r);
    return out;
}

} // namespace

TEST_CASE("Stress-Validierung: Winkelhalter, Spannungsmaximum an der inneren Ecke und Balkenabschätzung", "[Engine][Stress][Validation]")
{
    const double Lx = 50., t = 8., bw = 10., F = 50., E = 2000., nu = 0.3;
    const double I = bw * t * t * t / 12., A = bw * t, G = E / (2 * (1 + nu)), kap = 10. * (1 + nu) / (12. + 11. * nu);
    auto defl = [&](double l) { return F * l * l * l / (3 * E * I) + F * l / (kap * G * A); };
    const double sigma_corner = 6. * F * (Lx - t) / (bw * t * t); // 19,7 MPa, Sicherheit 2,03
    const double lo = defl(Lx - t), hi = defl(Lx);
    std::vector<BracketRun> runs;
    for (double voxel : { 2., 1., 0.5 })
        runs.push_back(run_bracket(voxel, sigma_corner, lo, hi));
    if (full_run())
        runs.push_back(run_bracket(0.25, sigma_corner, lo, hi));
    for (const BracketRun& r : runs) {
        REQUIRE(r.converged);
        CHECK(r.corner_distance <= r.voxel + 1e-9); // Maximum liegt in der Zelle an der inneren Ecke
        CHECK(r.tip > lo);                          // Durchbiegung zwischen den Balkenschranken
        CHECK(r.tip < hi);
        CHECK(r.max_vm / sigma_corner > 0.6);       // Größenordnung der Balkenspannung ...
        CHECK(r.max_vm / sigma_corner < 2.2);       // ... Gemessen 0,71 / 1,00 / 1,33 / 1,75 (Singularität, wächst)
        CHECK(std::abs(r.arm_error_top) < 0.08);    // Gemessen -6,7 / -2,7 / -1,2 / -0,5 %
        CHECK(std::abs(r.arm_error_top - r.arm_error_bottom) < 0.01); // Zug- und Druckseite symmetrisch
    }
    // Die Singularität zeigt sich als Wachstum bei jeder Verfeinerung (gemessen etwa das 1,3- bis 1,4-fache je Halbierung).
    for (size_t i = 1; i < runs.size(); ++i) {
        CHECK(runs[i].max_vm > 1.15 * runs[i - 1].max_vm);
        CHECK(runs[i].safety < runs[i - 1].safety);
        CHECK(std::abs(runs[i].arm_error_top) < std::abs(runs[i - 1].arm_error_top)); // der Arm konvergiert dagegen
    }
}
