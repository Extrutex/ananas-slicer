#include "Stress.hpp"

#include "Engine.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>

namespace Slic3r::engine {

namespace {

using Mat6  = Eigen::Matrix<double, 6, 6>;
using Mat24 = Eigen::Matrix<double, 24, 24, Eigen::RowMajor>;

constexpr uint32_t NONE = 0xFFFFFFFFu;

[[noreturn]] void invalid(const std::string& message)
{
    throw EngineError(ErrorCode::InvalidParams, message);
}

[[noreturn]] void failed(const std::string& message)
{
    throw EngineError(ErrorCode::EngineError, message);
}

// ---- the grid -------------------------------------------------------------------------------------------

struct Grid
{
    size_t nx{ 0 }, ny{ 0 }, nz{ 0 };
    double h{ 1. };
    Vec3d  origin{ Vec3d::Zero() };

    size_t cells() const { return nx * ny * nz; }
    size_t cell(size_t i, size_t j, size_t k) const { return i + nx * (j + ny * k); }
    size_t node(size_t i, size_t j, size_t k) const { return i + (nx + 1) * (j + (ny + 1) * k); }
    size_t node_count() const { return (nx + 1) * (ny + 1) * (nz + 1); }
    Vec3d  node_position(size_t i, size_t j, size_t k) const { return origin + h * Vec3d(double(i), double(j), double(k)); }
    Vec3d  cell_center(size_t i, size_t j, size_t k) const { return origin + h * Vec3d(i + 0.5, j + 0.5, k + 0.5); }
};

// A closed surface has every edge in exactly two triangles. Vertices are welded by position first, so a mesh
// that lists a vertex twice is not mistaken for an open one.
bool is_watertight(const indexed_triangle_set& mesh)
{
    std::map<std::array<float, 3>, uint32_t> welded;
    std::vector<uint32_t>                    id(mesh.vertices.size());
    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
        const auto& p = mesh.vertices[v];
        id[v]         = welded.try_emplace({ p.x(), p.y(), p.z() }, uint32_t(welded.size())).first->second;
    }
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const auto& t : mesh.indices)
        for (int e = 0; e < 3; ++e) {
            uint32_t a = id[t[e]], b = id[t[(e + 1) % 3]];
            if (a == b)
                continue; // a degenerate triangle edge carries no surface
            ++edges[{ std::min(a, b), std::max(a, b) }];
        }
    return std::all_of(edges.begin(), edges.end(), [](const auto& e) { return e.second == 2; });
}

// Fills `solid` with the cells whose centre lies inside the closed mesh: every (x, y) column is crossed by a
// vertical line, and the cells between the 1st and 2nd, 3rd and 4th ... crossing are inside.
void voxelize(const indexed_triangle_set& mesh, const Grid& grid, std::vector<uint8_t>& solid)
{
    // The column centres are nudged off the grid lines so a line never runs along a triangle edge.
    const double ex = 0.000123456 * grid.h, ey = 0.000234567 * grid.h;

    struct Hit
    {
        uint32_t column;
        double   z;
    };
    std::vector<Hit> hits;
    const auto       clamp_index = [](double v, size_t n) { return size_t(std::clamp(v, 0., double(n) - 1.)); };
    for (const auto& t : mesh.indices) {
        const Vec3d a = mesh.vertices[t[0]].cast<double>(), b = mesh.vertices[t[1]].cast<double>(), c = mesh.vertices[t[2]].cast<double>();
        const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
        if (std::abs(d) < 1e-18)
            continue; // vertical or degenerate: the vertical line cannot cross it
        const double min_x = std::min({ a.x(), b.x(), c.x() }), max_x = std::max({ a.x(), b.x(), c.x() });
        const double min_y = std::min({ a.y(), b.y(), c.y() }), max_y = std::max({ a.y(), b.y(), c.y() });
        const double i0 = std::ceil((min_x - grid.origin.x() - ex) / grid.h - 0.5), i1 = std::floor((max_x - grid.origin.x() - ex) / grid.h - 0.5);
        const double j0 = std::ceil((min_y - grid.origin.y() - ey) / grid.h - 0.5), j1 = std::floor((max_y - grid.origin.y() - ey) / grid.h - 0.5);
        if (i1 < i0 || j1 < j0)
            continue;
        for (size_t j = clamp_index(j0, grid.ny); j <= clamp_index(j1, grid.ny); ++j)
            for (size_t i = clamp_index(i0, grid.nx); i <= clamp_index(i1, grid.nx); ++i) {
                const double px = grid.origin.x() + (i + 0.5) * grid.h + ex, py = grid.origin.y() + (j + 0.5) * grid.h + ey;
                const double l1 = ((b.y() - c.y()) * (px - c.x()) + (c.x() - b.x()) * (py - c.y())) / d;
                const double l2 = ((c.y() - a.y()) * (px - c.x()) + (a.x() - c.x()) * (py - c.y())) / d;
                const double l3 = 1. - l1 - l2;
                if (l1 < 0. || l2 < 0. || l3 < 0.)
                    continue;
                hits.push_back({ uint32_t(i + grid.nx * j), l1 * a.z() + l2 * b.z() + l3 * c.z() });
            }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& l, const Hit& r) { return l.column != r.column ? l.column < r.column : l.z < r.z; });
    for (size_t first = 0; first < hits.size();) {
        size_t end = first;
        while (end < hits.size() && hits[end].column == hits[first].column)
            ++end;
        if ((end - first) % 2 != 0)
            failed("the mesh is not watertight (a line through it crosses the surface an odd number of times)");
        const size_t column = hits[first].column;
        for (size_t p = first; p < end; p += 2) {
            const double k0 = std::ceil((hits[p].z - grid.origin.z()) / grid.h - 0.5);
            const double k1 = std::floor((hits[p + 1].z - grid.origin.z()) / grid.h - 0.5);
            for (double k = std::max(k0, 0.); k <= std::min(k1, double(grid.nz) - 1.); k += 1.)
                solid[column + grid.nx * grid.ny * size_t(k)] = 1;
        }
        first = end;
    }
}

// ---- the element ----------------------------------------------------------------------------------------

// Orthotropic elasticity, transversely isotropic about z, in Voigt order (xx, yy, zz, yz, xz, xy) with
// engineering shear strains. Built from the compliance matrix: E_z = f E, G_xz = G_yz = f G, nu_xz = nu.
Mat6 elasticity(const StressMaterial& m)
{
    const double E = m.youngs_modulus, nu = m.poisson_ratio, f = m.z_factor;
    Eigen::Matrix3d S;
    S << 1. / E, -nu / E, -nu / E,
        -nu / E, 1. / E, -nu / E,
        -nu / E, -nu / E, 1. / (f * E);
    const Eigen::LLT<Eigen::Matrix3d> llt(S);
    if (llt.info() != Eigen::Success)
        invalid("material.poisson_ratio and z_factor give a material that is not stable");
    const double G = E / (2. * (1. + nu));
    Mat6         D = Mat6::Zero();
    D.topLeftCorner<3, 3>() = S.inverse();
    D(3, 3)                 = f * G;
    D(4, 4)                 = f * G;
    D(5, 5)                 = G;
    return D;
}

// Strain-displacement matrix of the trilinear hexahedron of edge h at natural coordinates (xi, eta, zeta).
Eigen::Matrix<double, 6, 24> strain_matrix(double h, double xi, double eta, double zeta)
{
    Eigen::Matrix<double, 6, 24> B = Eigen::Matrix<double, 6, 24>::Zero();
    for (int k = 0; k < 8; ++k) {
        const double sx = (k & 1) ? 1. : -1., sy = (k & 2) ? 1. : -1., sz = (k & 4) ? 1. : -1.;
        const double dx = sx * (1 + sy * eta) * (1 + sz * zeta) / 8. * (2. / h);
        const double dy = sy * (1 + sx * xi) * (1 + sz * zeta) / 8. * (2. / h);
        const double dz = sz * (1 + sx * xi) * (1 + sy * eta) / 8. * (2. / h);
        B(0, 3 * k)     = dx;
        B(1, 3 * k + 1) = dy;
        B(2, 3 * k + 2) = dz;
        B(3, 3 * k + 1) = dz;
        B(3, 3 * k + 2) = dy;
        B(4, 3 * k)     = dz;
        B(4, 3 * k + 2) = dx;
        B(5, 3 * k)     = dy;
        B(5, 3 * k + 1) = dx;
    }
    return B;
}

Mat24 element_stiffness(double h, const Mat6& D)
{
    Mat24        K = Mat24::Zero();
    const double g = 1. / std::sqrt(3.), det = std::pow(h / 2., 3);
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c) {
                const auto B = strain_matrix(h, a ? g : -g, b ? g : -g, c ? g : -g);
                K += B.transpose() * D * B * det;
            }
    return K;
}

// ---- selection ------------------------------------------------------------------------------------------

double distance2_point_triangle(const Vec3d& p, const Vec3d& a, const Vec3d& b, const Vec3d& c)
{
    const Vec3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0 && d2 <= 0)
        return ap.squaredNorm();
    const Vec3d  bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3)
        return bp.squaredNorm();
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return (p - (a + ab * (d1 / (d1 - d3)))).squaredNorm();
    const Vec3d  cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6)
        return cp.squaredNorm();
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return (p - (a + ac * (d2 / (d2 - d6)))).squaredNorm();
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return (p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))))).squaredNorm();
    const double denom = 1. / (va + vb + vc);
    return (p - (a + ab * (vb * denom) + ac * (vc * denom))).squaredNorm();
}

// Everything the solver needs about the meshed body.
struct Body
{
    Grid                  grid;
    std::vector<uint8_t>  solid;
    std::vector<uint32_t> active;   // full node index -> solver node, NONE when no solid cell touches it
    std::vector<uint32_t> node_full; // solver node -> full node index
    std::vector<uint32_t> elem_cell; // element -> cell index
    std::vector<uint32_t> elem_nodes; // 8 solver nodes per element, local order x fastest
    std::vector<uint32_t> adjacent;   // 8 per solver node: the element in which the node has local index k, or NONE
};

bool solid_at(const Body& body, long i, long j, long k)
{
    const Grid& g = body.grid;
    return i >= 0 && j >= 0 && k >= 0 && i < long(g.nx) && j < long(g.ny) && k < long(g.nz) && body.solid[g.cell(i, j, k)];
}

void build_body(Body& body)
{
    const Grid& g = body.grid;
    std::vector<uint8_t> used(g.node_count(), 0);
    for (size_t k = 0; k < g.nz; ++k)
        for (size_t j = 0; j < g.ny; ++j)
            for (size_t i = 0; i < g.nx; ++i)
                if (body.solid[g.cell(i, j, k)])
                    for (int n = 0; n < 8; ++n)
                        used[g.node(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1))] = 1;
    body.active.assign(g.node_count(), NONE);
    for (size_t n = 0; n < used.size(); ++n)
        if (used[n]) {
            body.active[n] = uint32_t(body.node_full.size());
            body.node_full.push_back(uint32_t(n));
        }
    for (size_t k = 0; k < g.nz; ++k)
        for (size_t j = 0; j < g.ny; ++j)
            for (size_t i = 0; i < g.nx; ++i)
                if (body.solid[g.cell(i, j, k)]) {
                    body.elem_cell.push_back(uint32_t(g.cell(i, j, k)));
                    for (int n = 0; n < 8; ++n)
                        body.elem_nodes.push_back(body.active[g.node(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1))]);
                }
    body.adjacent.assign(body.node_full.size() * 8, NONE);
    for (size_t e = 0; e < body.elem_cell.size(); ++e)
        for (int n = 0; n < 8; ++n)
            body.adjacent[size_t(body.elem_nodes[e * 8 + n]) * 8 + n] = uint32_t(e);
}

// The solver nodes a support or a load acts on.
std::vector<uint8_t> select_nodes(const Body& body, const StressSelection& where, const std::vector<std::array<Vec3d, 3>>& triangles)
{
    const Grid&          g = body.grid;
    std::vector<uint8_t> chosen(body.node_full.size(), 0);
    const auto           node_ijk = [&](uint32_t full) {
        return std::array<size_t, 3>{ full % (g.nx + 1), (full / (g.nx + 1)) % (g.ny + 1), full / ((g.nx + 1) * (g.ny + 1)) };
    };

    if (where.box) {
        // Half a cell of slack: the voxel surface lies up to half a cell from the true one.
        const Vec3d lo = where.box->min.array() - 0.5 * g.h, hi = where.box->max.array() + 0.5 * g.h;
        for (size_t n = 0; n < body.node_full.size(); ++n) {
            const auto  ijk = node_ijk(body.node_full[n]);
            const Vec3d p   = g.node_position(ijk[0], ijk[1], ijk[2]);
            if ((p.array() >= lo.array()).all() && (p.array() <= hi.array()).all())
                chosen[n] = 1;
        }
    }

    // A node of the voxel surface lies within half a cell diagonal of the triangle it stands for.
    const double reach = 0.8661 * g.h, reach2 = reach * reach;
    for (const auto& tri : triangles) {
        Vec3d lo = tri[0].cwiseMin(tri[1]).cwiseMin(tri[2]).array() - reach, hi = tri[0].cwiseMax(tri[1]).cwiseMax(tri[2]).array() + reach;
        size_t first[3], last[3];
        bool   outside = false;
        const size_t count[3] = { g.nx + 1, g.ny + 1, g.nz + 1 };
        for (int d = 0; d < 3; ++d) {
            const double a = std::ceil((lo[d] - g.origin[d]) / g.h), b = std::floor((hi[d] - g.origin[d]) / g.h);
            if (b < 0 || a > double(count[d]) - 1 || b < a) {
                outside = true;
                break;
            }
            first[d] = size_t(std::max(a, 0.));
            last[d]  = size_t(std::min(b, double(count[d]) - 1));
        }
        if (outside)
            continue;
        for (size_t k = first[2]; k <= last[2]; ++k)
            for (size_t j = first[1]; j <= last[1]; ++j)
                for (size_t i = first[0]; i <= last[0]; ++i) {
                    const uint32_t node = body.active[g.node(i, j, k)];
                    if (node != NONE && !chosen[node] && distance2_point_triangle(g.node_position(i, j, k), tri[0], tri[1], tri[2]) <= reach2)
                        chosen[node] = 1;
                }
    }
    return chosen;
}

// Spreads a force over the chosen nodes. Surface quads whose four nodes are all chosen carry it in equal parts
// (a quarter of a quad's share to each corner: the consistent load of a uniform traction, so a corner node takes
// less than an interior one). A selection without such a quad (an edge, a single node) shares it equally.
void distribute_load(const Body& body, const std::vector<uint8_t>& chosen, const Vec3d& force, std::vector<double>& f)
{
    const Grid& g = body.grid;
    std::vector<std::array<uint32_t, 4>> quads;
    for (size_t k = 0; k < g.nz; ++k)
        for (size_t j = 0; j < g.ny; ++j)
            for (size_t i = 0; i < g.nx; ++i) {
                if (!body.solid[g.cell(i, j, k)])
                    continue;
                const long c[3] = { long(i), long(j), long(k) };
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        long nb[3] = { c[0], c[1], c[2] };
                        nb[axis] += side ? 1 : -1;
                        if (solid_at(body, nb[0], nb[1], nb[2]))
                            continue;
                        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                        std::array<uint32_t, 4> q;
                        bool                    all = true;
                        for (int s = 0; s < 4 && all; ++s) {
                            size_t idx[3] = { size_t(c[0]), size_t(c[1]), size_t(c[2]) };
                            idx[axis] += side;
                            idx[u] += s & 1;
                            idx[v] += (s >> 1) & 1;
                            q[s] = body.active[g.node(idx[0], idx[1], idx[2])];
                            all  = chosen[q[s]] != 0;
                        }
                        if (all)
                            quads.push_back(q);
                    }
            }
    if (!quads.empty()) {
        const Vec3d share = force / (4. * double(quads.size()));
        for (const auto& q : quads)
            for (uint32_t n : q)
                for (int a = 0; a < 3; ++a)
                    f[3 * size_t(n) + a] += share[a];
        return;
    }
    const size_t count = size_t(std::count(chosen.begin(), chosen.end(), 1));
    for (size_t n = 0; n < chosen.size(); ++n)
        if (chosen[n])
            for (int a = 0; a < 3; ++a)
                f[3 * n + a] += force[a] / double(count);
}

// ---- the solver -----------------------------------------------------------------------------------------

struct Operator
{
    const Body&           body;
    std::vector<double>   ke;    // 24 x 24, row-major
    const std::vector<uint8_t>& fixed; // per dof

    // y = K x. With `masked`, the fixed dofs of x are taken as zero and those of y are zeroed (the operator of
    // the free dofs); without, y carries the reactions at the fixed dofs.
    void apply(const std::vector<double>& x, std::vector<double>& y, bool masked) const
    {
        const size_t n_nodes = body.node_full.size();
        tbb::parallel_for(tbb::blocked_range<size_t>(0, n_nodes, 1024), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t n = range.begin(); n != range.end(); ++n) {
                double acc[3] = { 0., 0., 0. };
                for (int k = 0; k < 8; ++k) {
                    const uint32_t e = body.adjacent[n * 8 + k];
                    if (e == NONE)
                        continue;
                    const uint32_t* nodes = &body.elem_nodes[size_t(e) * 8];
                    for (int j = 0; j < 8; ++j) {
                        const double* xm = &x[3 * size_t(nodes[j])];
                        for (int a = 0; a < 3; ++a) {
                            const double* row = &ke[size_t(3 * k + a) * 24 + 3 * j];
                            acc[a] += row[0] * xm[0] + row[1] * xm[1] + row[2] * xm[2];
                        }
                    }
                }
                for (int a = 0; a < 3; ++a)
                    y[3 * n + a] = masked && fixed[3 * n + a] ? 0. : acc[a];
            }
        });
    }
};

double dot(const std::vector<double>& a, const std::vector<double>& b)
{
    double s = 0.;
    for (size_t i = 0; i < a.size(); ++i)
        s += a[i] * b[i];
    return s;
}

} // namespace

StressResult solve_stress(const StressInput& input, const StressProgress& progress)
{
    const auto report = [&](int percent, const std::string& message) {
        if (progress)
            progress(percent, message);
    };

    // ---- input checks
    const StressMaterial& mat = input.material;
    if (!(mat.youngs_modulus > 0.) || !std::isfinite(mat.youngs_modulus))
        invalid("material.youngs_modulus must be above 0");
    if (!(mat.poisson_ratio >= 0. && mat.poisson_ratio < 0.5))
        invalid("material.poisson_ratio must be at least 0 and below 0.5");
    if (!(mat.yield_strength > 0.) || !std::isfinite(mat.yield_strength))
        invalid("material.yield_strength must be above 0");
    if (!(mat.z_factor >= 0.05 && mat.z_factor <= 1.))
        invalid("material.z_factor must be between 0.05 and 1");
    if (!(input.voxel_size > 0.) || !std::isfinite(input.voxel_size))
        invalid("voxel_size must be above 0");
    if (!(input.tolerance > 0. && input.tolerance < 1.))
        invalid("tolerance must be between 0 and 1");
    if (input.supports.empty())
        invalid("no support given: the body would be free to move");
    if (input.loads.empty())
        invalid("no load given");
    for (const StressLoad& load : input.loads)
        if (!load.force.allFinite())
            invalid("a load force must be finite");

    // ---- the grid
    size_t triangle_total = 0;
    BoundingBoxf3 box;
    for (const indexed_triangle_set& part : input.parts) {
        triangle_total += part.indices.size();
        for (const auto& t : part.indices)
            for (int c = 0; c < 3; ++c)
                box.merge(part.vertices[t[c]].cast<double>());
    }
    if (triangle_total == 0 || !box.defined)
        failed("the model has no solid geometry");
    auto  owner = std::make_unique<Body>();
    Body& b     = *owner;
    Grid& grid  = b.grid;
    grid.h      = input.voxel_size;
    grid.origin = box.min;
    const Vec3d size = box.size();
    double      cells = 1.;
    size_t      n[3];
    for (int d = 0; d < 3; ++d) {
        n[d] = size_t(std::max(1., std::ceil(size[d] / grid.h - 1e-6)));
        cells *= double(n[d]);
    }
    if (cells > double(input.max_cells))
        invalid("voxel_size is too small for this model: " + std::to_string(size_t(cells)) + " cells, at most " + std::to_string(input.max_cells));
    grid.nx = n[0];
    grid.ny = n[1];
    grid.nz = n[2];

    // ---- voxelization
    report(0, "Voxelizing");
    for (const indexed_triangle_set& part : input.parts)
        if (!is_watertight(part))
            failed("the mesh is not watertight (open or non-manifold edges)");
    b.solid.assign(grid.cells(), 0);
    for (const indexed_triangle_set& part : input.parts)
        voxelize(part, grid, b.solid);
    const size_t solid_cells = size_t(std::count(b.solid.begin(), b.solid.end(), 1));
    if (solid_cells == 0)
        failed("the voxel model is empty: the model is thinner than the voxel size");
    build_body(b);
    const size_t n_nodes = b.node_full.size(), n_dofs = 3 * n_nodes;

    // ---- supports and loads
    report(8, "Applying supports and loads");
    std::vector<std::array<Vec3d, 3>> all_triangles;
    all_triangles.reserve(triangle_total);
    for (const indexed_triangle_set& part : input.parts)
        for (const auto& t : part.indices)
            all_triangles.push_back({ part.vertices[t[0]].cast<double>(), part.vertices[t[1]].cast<double>(), part.vertices[t[2]].cast<double>() });
    const auto pick = [&](const StressSelection& where, const char* what, size_t index) {
        std::vector<std::array<Vec3d, 3>> chosen;
        for (size_t id : where.triangles) {
            if (id >= all_triangles.size())
                invalid(std::string(what) + " " + std::to_string(index) + ": triangle " + std::to_string(id) + " does not exist");
            chosen.push_back(all_triangles[id]);
        }
        if (chosen.empty() && !where.box)
            invalid(std::string(what) + " " + std::to_string(index) + " selects nothing (give triangles or a box)");
        std::vector<uint8_t> nodes = select_nodes(b, where, chosen);
        if (std::none_of(nodes.begin(), nodes.end(), [](uint8_t v) { return v != 0; }))
            invalid(std::string(what) + " " + std::to_string(index) + " selects no material");
        return nodes;
    };

    std::vector<uint8_t> fixed(n_dofs, 0);
    for (size_t s = 0; s < input.supports.size(); ++s) {
        const std::vector<uint8_t> nodes = pick(input.supports[s].where, "support", s);
        for (size_t node = 0; node < n_nodes; ++node)
            if (nodes[node])
                for (int a = 0; a < 3; ++a)
                    if (input.supports[s].axes[a])
                        fixed[3 * node + a] = 1;
    }
    std::vector<double> f(n_dofs, 0.);
    for (size_t l = 0; l < input.loads.size(); ++l)
        distribute_load(b, pick(input.loads[l].where, "load", l), input.loads[l].force, f);

    StressResult result;
    result.size       = { grid.nx, grid.ny, grid.nz };
    result.voxel_size = grid.h;
    result.origin     = grid.origin;
    result.solid_cells = solid_cells;
    result.nodes      = n_nodes;
    result.fixed_dofs = size_t(std::count(fixed.begin(), fixed.end(), 1));
    for (size_t i = 0; i < n_dofs; ++i)
        if (fixed[i])
            f[i] = 0.; // a load on a held node goes into the support
        else
            result.applied_force[i % 3] += f[i];
    const double f_norm = std::sqrt(dot(f, f));
    if (f_norm <= 0.)
        invalid("no load reaches a free node (all loaded nodes are held by a support, or the force is zero)");

    // Every connected piece needs a hold, otherwise the system is singular.
    {
        std::vector<uint32_t> parent(n_nodes);
        for (size_t i = 0; i < n_nodes; ++i)
            parent[i] = uint32_t(i);
        const auto find = [&](uint32_t x) {
            while (parent[x] != x)
                x = parent[x] = parent[parent[x]];
            return x;
        };
        for (size_t e = 0; e < b.elem_cell.size(); ++e)
            for (int k = 1; k < 8; ++k)
                parent[find(b.elem_nodes[e * 8])] = find(b.elem_nodes[e * 8 + k]);
        std::vector<uint8_t> held(n_nodes, 0);
        for (size_t node = 0; node < n_nodes; ++node)
            if (fixed[3 * node] || fixed[3 * node + 1] || fixed[3 * node + 2])
                held[find(uint32_t(node))] = 1;
        for (size_t node = 0; node < n_nodes; ++node)
            if (find(uint32_t(node)) == node && !held[node])
                failed("a separate piece of the model has no support (the voxel model falls into several pieces, one of them free)");
    }

    // ---- conjugate gradient
    const Mat6  D  = elasticity(mat);
    const Mat24 Ke = element_stiffness(grid.h, D);
    std::vector<double> ke(Ke.data(), Ke.data() + 576);
    const Operator op{ b, ke, fixed };

    std::vector<double> inv_diag(n_dofs, 1.);
    for (size_t node = 0; node < n_nodes; ++node)
        for (int a = 0; a < 3; ++a) {
            double d = 0.;
            for (int k = 0; k < 8; ++k)
                if (b.adjacent[node * 8 + k] != NONE)
                    d += Ke(3 * k + a, 3 * k + a);
            inv_diag[3 * node + a] = fixed[3 * node + a] ? 1. : 1. / d;
        }

    report(15, "Solving");
    std::vector<double> u(n_dofs, 0.), r = f, z(n_dofs), p(n_dofs), q(n_dofs);
    for (size_t i = 0; i < n_dofs; ++i)
        z[i] = inv_diag[i] * r[i];
    p = z;
    double rz = dot(r, z), r_norm = f_norm;
    int    last_percent = 15;
    while (result.iterations < input.max_iterations) {
        if (r_norm <= input.tolerance * f_norm) {
            result.converged = true;
            break;
        }
        op.apply(p, q, true);
        const double pq = dot(p, q);
        if (!(pq > 0.))
            break; // the operator is not positive on p: a mechanism
        const double alpha = rz / pq;
        for (size_t i = 0; i < n_dofs; ++i) {
            u[i] += alpha * p[i];
            r[i] -= alpha * q[i];
            z[i] = inv_diag[i] * r[i];
        }
        const double rz_new = dot(r, z);
        const double beta   = rz_new / rz;
        rz                  = rz_new;
        for (size_t i = 0; i < n_dofs; ++i)
            p[i] = z[i] + beta * p[i];
        r_norm = std::sqrt(dot(r, r));
        ++result.iterations;
        if (result.iterations % 25 == 0) {
            const double done = std::log(1. / std::max(r_norm / f_norm, 1e-300)) / std::log(1. / input.tolerance);
            const int percent = 15 + int(std::clamp(done, 0., 1.) * 78.);
            if (percent > last_percent) {
                last_percent = percent;
                report(percent, "Solving, iteration " + std::to_string(result.iterations));
            }
        }
    }
    result.residual = r_norm / f_norm;
    if (!result.converged && r_norm <= input.tolerance * f_norm)
        result.converged = true;
    if (!result.converged)
        failed("the solver did not converge after " + std::to_string(result.iterations) + " iterations (relative residual " +
               std::to_string(result.residual) + "); the supports probably leave the body free to move or to rotate");

    // ---- results
    report(94, "Evaluating stress");
    {
        std::vector<double> reaction(n_dofs);
        op.apply(u, reaction, false);
        for (size_t i = 0; i < n_dofs; ++i)
            if (fixed[i])
                result.reaction[i % 3] += reaction[i];
    }

    const size_t cells_total = grid.cells();
    result.solid            = b.solid;
    result.von_mises.assign(cells_total, 0.f);
    result.safety.assign(cells_total, 0.f);
    result.displacement.assign(cells_total, 0.f);
    result.displacement_vec.assign(cells_total * 3, 0.f);
    const auto DB = (D * strain_matrix(grid.h, 0., 0., 0.)).eval();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, b.elem_cell.size(), 512), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t e = range.begin(); e != range.end(); ++e) {
            Eigen::Matrix<double, 24, 1> ue;
            Vec3d                        mean = Vec3d::Zero();
            for (int k = 0; k < 8; ++k) {
                const Vec3d v(u[3 * size_t(b.elem_nodes[e * 8 + k])], u[3 * size_t(b.elem_nodes[e * 8 + k]) + 1],
                              u[3 * size_t(b.elem_nodes[e * 8 + k]) + 2]);
                ue.segment<3>(3 * k) = v;
                mean += v / 8.;
            }
            const Eigen::Matrix<double, 6, 1> s = DB * ue;
            const double vm = std::sqrt(std::max(0., s[0] * s[0] + s[1] * s[1] + s[2] * s[2] - s[0] * s[1] - s[1] * s[2] - s[2] * s[0] +
                                                         3. * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5])));
            double safety = vm > 0. ? mat.yield_strength / vm : STRESS_SAFETY_CAP;
            // Across the layers only a tensile stress threatens the bond.
            if (mat.z_factor < 1. && s[2] > 0.)
                safety = std::min(safety, mat.z_factor * mat.yield_strength / s[2]);
            const size_t cell = b.elem_cell[e];
            result.von_mises[cell]     = float(vm);
            result.safety[cell]        = float(std::min(safety, STRESS_SAFETY_CAP));
            result.displacement[cell]  = float(mean.norm());
            for (int a = 0; a < 3; ++a)
                result.displacement_vec[3 * cell + a] = float(mean[a]);
        }
    });

    result.min_safety = STRESS_SAFETY_CAP;
    for (size_t k = 0; k < grid.nz; ++k)
        for (size_t j = 0; j < grid.ny; ++j)
            for (size_t i = 0; i < grid.nx; ++i) {
                const size_t cell = grid.cell(i, j, k);
                if (!b.solid[cell])
                    continue;
                if (result.von_mises[cell] > result.max_von_mises || result.max_von_mises == 0.) {
                    result.max_von_mises    = result.von_mises[cell];
                    result.max_von_mises_at = grid.cell_center(i, j, k);
                }
                if (result.safety[cell] < result.min_safety) {
                    result.min_safety    = result.safety[cell];
                    result.min_safety_at = grid.cell_center(i, j, k);
                }
            }
    for (size_t node = 0; node < n_nodes; ++node) {
        const double len = Vec3d(u[3 * node], u[3 * node + 1], u[3 * node + 2]).norm();
        if (len > result.max_displacement) {
            const uint32_t full = b.node_full[node];
            result.max_displacement    = len;
            result.max_displacement_at = grid.node_position(full % (grid.nx + 1), (full / (grid.nx + 1)) % (grid.ny + 1),
                                                            full / ((grid.nx + 1) * (grid.ny + 1)));
        }
    }
    report(100, "Finished");
    return result;
}

} // namespace Slic3r::engine
