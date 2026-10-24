#include "Stress.hpp"

#include "Engine.hpp"

#include <Eigen/Eigenvalues>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>

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

// The wall clock of one analysis, counted from the start of solve_stress. Every long loop calls tick(), which looks
// at the clock on the first call and then once every 256 calls.
struct Deadline
{
    std::chrono::steady_clock::time_point start{ std::chrono::steady_clock::now() };
    double                                seconds{ 0. };
    uint32_t                              counter{ 0 };

    bool expired() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds; }
    void tick(const char* phase)
    {
        if ((counter++ & 0xFFu) == 0 && expired())
            failed(std::string("the time limit of ") + format_seconds(seconds) + " s was reached while " + phase +
                   "; use a larger voxel_size or a higher max_seconds");
    }
    static std::string format_seconds(double v)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%g", v);
        return text;
    }
};

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
bool is_watertight(const indexed_triangle_set& mesh, Deadline& deadline)
{
    std::map<std::array<float, 3>, uint32_t> welded;
    std::vector<uint32_t>                    id(mesh.vertices.size());
    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
        deadline.tick("checking the mesh");
        const auto& p = mesh.vertices[v];
        id[v]         = welded.try_emplace({ p.x(), p.y(), p.z() }, uint32_t(welded.size())).first->second;
    }
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const auto& t : mesh.indices) {
        deadline.tick("checking the mesh");
        for (int e = 0; e < 3; ++e) {
            uint32_t a = id[t[e]], b = id[t[(e + 1) % 3]];
            if (a == b)
                continue; // a degenerate triangle edge carries no surface
            ++edges[{ std::min(a, b), std::max(a, b) }];
        }
    }
    return std::all_of(edges.begin(), edges.end(), [](const auto& e) { return e.second == 2; });
}

// Fills `solid` with the cells whose centre lies inside the closed mesh: every (x, y) column is crossed by a
// vertical line, and the cells between the 1st and 2nd, 3rd and 4th ... crossing are inside.
void voxelize(const indexed_triangle_set& mesh, const Grid& grid, std::vector<uint8_t>& solid, size_t max_hits, Deadline& deadline)
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
        deadline.tick("voxelizing");
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
        for (size_t j = clamp_index(j0, grid.ny); j <= clamp_index(j1, grid.ny); ++j) {
            deadline.tick("voxelizing");
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
        if (hits.size() > max_hits)
            invalid("the mesh is too complex for this grid or the memory limit (more than " + std::to_string(max_hits) +
                    " surface crossings); use a larger voxel_size");
    }
    deadline.tick("voxelizing");
    std::sort(hits.begin(), hits.end(), [](const Hit& l, const Hit& r) { return l.column != r.column ? l.column < r.column : l.z < r.z; });
    for (size_t first = 0; first < hits.size();) {
        deadline.tick("voxelizing");
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

void build_body(Body& body, Deadline& deadline)
{
    const Grid& g = body.grid;
    std::vector<uint8_t> used(g.node_count(), 0);
    for (size_t k = 0; k < g.nz; ++k)
        for (size_t j = 0; j < g.ny; ++j) {
            deadline.tick("building the mesh");
            for (size_t i = 0; i < g.nx; ++i)
                if (body.solid[g.cell(i, j, k)])
                    for (int n = 0; n < 8; ++n)
                        used[g.node(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1))] = 1;
        }
    body.active.assign(g.node_count(), NONE);
    for (size_t n = 0; n < used.size(); ++n)
        if (used[n]) {
            body.active[n] = uint32_t(body.node_full.size());
            body.node_full.push_back(uint32_t(n));
        }
    for (size_t k = 0; k < g.nz; ++k)
        for (size_t j = 0; j < g.ny; ++j) {
            deadline.tick("building the mesh");
            for (size_t i = 0; i < g.nx; ++i)
                if (body.solid[g.cell(i, j, k)]) {
                    body.elem_cell.push_back(uint32_t(g.cell(i, j, k)));
                    for (int n = 0; n < 8; ++n)
                        body.elem_nodes.push_back(body.active[g.node(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1))]);
                }
        }
    body.adjacent.assign(body.node_full.size() * 8, NONE);
    for (size_t e = 0; e < body.elem_cell.size(); ++e) {
        deadline.tick("building the mesh");
        for (int n = 0; n < 8; ++n)
            body.adjacent[size_t(body.elem_nodes[e * 8 + n]) * 8 + n] = uint32_t(e);
    }
}

// Triangles of all model parts under one running index (parts in order), without a copy of them.
struct TriangleSource
{
    const std::vector<indexed_triangle_set>& parts;
    std::vector<size_t>                      offset; // first triangle index of each part, and the total at the end
    std::vector<bool>                        inverted; // per part: the mesh is wound inside out (negative volume)

    explicit TriangleSource(const std::vector<indexed_triangle_set>& p) : parts(p)
    {
        offset.push_back(0);
        for (const auto& part : parts) {
            offset.push_back(offset.back() + part.indices.size());
            inverted.push_back(its_volume(part) < 0.f);
        }
    }
    // Whether the part of triangle `id` is wound inside out, so that its normal points into the material.
    bool is_inverted(size_t id) const { return inverted[size_t(std::upper_bound(offset.begin(), offset.end(), id) - offset.begin()) - 1]; }
    size_t count() const { return offset.back(); }
    std::array<Vec3d, 3> at(size_t id) const
    {
        const size_t p = size_t(std::upper_bound(offset.begin(), offset.end(), id) - offset.begin()) - 1;
        const auto&  t = parts[p].indices[id - offset[p]];
        return { parts[p].vertices[t[0]].cast<double>(), parts[p].vertices[t[1]].cast<double>(), parts[p].vertices[t[2]].cast<double>() };
    }
};

using Quad = std::array<uint32_t, 4>;

// What a support or a load acts on: solver nodes, and the surface quads (the cell faces on the voxel surface) that
// carry a load.
struct Selected
{
    std::vector<uint8_t> nodes;
    std::vector<Quad>    quads;
};

std::array<size_t, 3> node_ijk(const Grid& g, uint32_t full)
{
    return { full % (g.nx + 1), (full / (g.nx + 1)) % (g.ny + 1), full / ((g.nx + 1) * (g.ny + 1)) };
}

// The four solver nodes of face `side` (0 lower, 1 upper) of cell c across `axis`, in a fixed order.
Quad face_quad(const Body& body, const long c[3], int axis, int side)
{
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    Quad      q;
    for (int s = 0; s < 4; ++s) {
        size_t idx[3] = { size_t(c[0]), size_t(c[1]), size_t(c[2]) };
        idx[axis] += side;
        idx[u] += s & 1;
        idx[v] += (s >> 1) & 1;
        q[s] = body.active[body.grid.node(idx[0], idx[1], idx[2])];
    }
    return q;
}

bool face_exposed(const Body& body, const long c[3], int axis, int side)
{
    long nb[3] = { c[0], c[1], c[2] };
    nb[axis] += side ? 1 : -1;
    return !solid_at(body, nb[0], nb[1], nb[2]);
}

// The solver nodes and surface quads a support or a load acts on. A box takes the nodes inside it (half a cell of
// slack) and the surface quads whose four nodes are all inside. Triangles take the surface quads that face the
// triangle (their normal within 60 degrees of it) and whose centre lies within reach of it: only the outer layer of
// the voxel surface, so a face that lies between two grid planes does not pull in the side faces. A triangle
// selection that finds no quad at all (a triangle much smaller than a cell) falls back to the nodes near the triangle.
Selected select(const Body& body, const StressSelection& where, const std::vector<size_t>& ids, const TriangleSource& source,
                size_t max_quads, Deadline& deadline)
{
    const Grid& g = body.grid;
    Selected    out;
    out.nodes.assign(body.node_full.size(), 0);
    std::vector<uint8_t> marked(g.cells(), 0); // per cell one bit for each of its 6 faces already taken
    const auto add = [&](size_t cell, int face, const Quad& q) {
        if (marked[cell] & (1u << face))
            return;
        marked[cell] |= uint8_t(1u << face);
        if (out.quads.size() >= max_quads)
            invalid("the selection covers too many surface faces for the memory limit; use a larger voxel_size");
        out.quads.push_back(q);
        for (uint32_t n : q)
            out.nodes[n] = 1;
    };

    if (where.box) {
        // Half a cell of slack: the voxel surface lies up to half a cell from the true one.
        const Vec3d          lo = where.box->min.array() - 0.5 * g.h, hi = where.box->max.array() + 0.5 * g.h;
        std::vector<uint8_t> inside(body.node_full.size(), 0);
        for (size_t n = 0; n < body.node_full.size(); ++n) {
            deadline.tick("selecting supports and loads");
            const auto  ijk = node_ijk(g, body.node_full[n]);
            const Vec3d p   = g.node_position(ijk[0], ijk[1], ijk[2]);
            if ((p.array() >= lo.array()).all() && (p.array() <= hi.array()).all())
                inside[n] = out.nodes[n] = 1;
        }
        for (size_t k = 0; k < g.nz; ++k)
            for (size_t j = 0; j < g.ny; ++j) {
                deadline.tick("selecting supports and loads");
                for (size_t i = 0; i < g.nx; ++i) {
                    if (!body.solid[g.cell(i, j, k)])
                        continue;
                    const long c[3] = { long(i), long(j), long(k) };
                    for (int axis = 0; axis < 3; ++axis)
                        for (int side = 0; side < 2; ++side) {
                            if (!face_exposed(body, c, axis, side))
                                continue;
                            const Quad q = face_quad(body, c, axis, side);
                            if (inside[q[0]] && inside[q[1]] && inside[q[2]] && inside[q[3]])
                                add(g.cell(i, j, k), axis * 2 + side, q);
                        }
                }
            }
    }

    const double reach = 0.8661 * g.h, reach2 = reach * reach;
    for (size_t id : ids) {
        const auto  tri = source.at(id);
        Vec3d       normal = (tri[1] - tri[0]).cross(tri[2] - tri[0]);
        const double len   = normal.norm();
        if (len > 0.)
            normal /= len;
        if (source.is_inverted(id))
            normal = -normal; // the outward normal, also for a mesh wound inside out
        const Vec3d lo = tri[0].cwiseMin(tri[1]).cwiseMin(tri[2]).array() - reach, hi = tri[0].cwiseMax(tri[1]).cwiseMax(tri[2]).array() + reach;
        size_t       first[3], last[3];
        bool         outside = false;
        const size_t count[3] = { g.nx, g.ny, g.nz };
        for (int d = 0; d < 3; ++d) { // the cells whose centre lies in the enlarged box of the triangle
            const double a = std::ceil((lo[d] - g.origin[d]) / g.h - 0.5), b = std::floor((hi[d] - g.origin[d]) / g.h - 0.5);
            if (b < 0 || a > double(count[d]) - 1 || b < a) {
                outside = true;
                break;
            }
            first[d] = size_t(std::max(a, 0.));
            last[d]  = size_t(std::min(b, double(count[d]) - 1));
        }
        if (outside || len <= 0.)
            continue;
        for (size_t k = first[2]; k <= last[2]; ++k)
            for (size_t j = first[1]; j <= last[1]; ++j) {
                deadline.tick("selecting supports and loads");
                for (size_t i = first[0]; i <= last[0]; ++i) {
                    if (!body.solid[g.cell(i, j, k)])
                        continue;
                    const long c[3] = { long(i), long(j), long(k) };
                    for (int axis = 0; axis < 3; ++axis) {
                        if (std::abs(normal[axis]) < 0.5)
                            continue;
                        for (int side = 0; side < 2; ++side) {
                            // Only the face that looks the way the triangle looks: the back of a one-cell wall is not selected.
                            if (normal[axis] * (side ? 1. : -1.) < 0.5)
                                continue;
                            if (!face_exposed(body, c, axis, side))
                                continue;
                            Vec3d centre = g.cell_center(i, j, k);
                            centre[axis] += (side ? 0.5 : -0.5) * g.h;
                            if (distance2_point_triangle(centre, tri[0], tri[1], tri[2]) <= reach2)
                                add(g.cell(i, j, k), axis * 2 + side, face_quad(body, c, axis, side));
                        }
                    }
                }
            }
    }

    if (!ids.empty() && out.quads.empty()) {
        // No surface face found, neither by box nor by triangle: the nodes within reach of the triangles.
        for (size_t id : ids) {
            const auto  tri = source.at(id);
            const Vec3d lo = tri[0].cwiseMin(tri[1]).cwiseMin(tri[2]).array() - reach, hi = tri[0].cwiseMax(tri[1]).cwiseMax(tri[2]).array() + reach;
            size_t       first[3], last[3];
            bool         outside = false;
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
                for (size_t j = first[1]; j <= last[1]; ++j) {
                    deadline.tick("selecting supports and loads");
                    for (size_t i = first[0]; i <= last[0]; ++i) {
                        const uint32_t node = body.active[g.node(i, j, k)];
                        if (node != NONE && !out.nodes[node] && distance2_point_triangle(g.node_position(i, j, k), tri[0], tri[1], tri[2]) <= reach2)
                            out.nodes[node] = 1;
                    }
                }
        }
    }
    return out;
}

// Spreads a force over the selection. Surface quads carry it in equal parts (a quarter of a quad's share to each
// corner: the consistent load of a uniform traction, so a corner node takes less than an interior one). A selection
// without a quad (an edge, a single node) shares it equally between its nodes.
void distribute_load(const Selected& selected, const Vec3d& force, std::vector<double>& f)
{
    if (!selected.quads.empty()) {
        const Vec3d share = force / (4. * double(selected.quads.size()));
        for (const auto& q : selected.quads)
            for (uint32_t n : q)
                for (int a = 0; a < 3; ++a)
                    f[3 * size_t(n) + a] += share[a];
        return;
    }
    const size_t count = size_t(std::count(selected.nodes.begin(), selected.nodes.end(), 1));
    for (size_t n = 0; n < selected.nodes.size(); ++n)
        if (selected.nodes[n])
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

// Number of face-connected groups of solid cells.
size_t face_components(const Body& body, Deadline& deadline)
{
    const Grid&           g = body.grid;
    std::vector<uint32_t> parent(g.cells());
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = uint32_t(i);
    const auto find = [&](uint32_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    size_t count = 0;
    for (size_t k = 0; k < g.nz; ++k)
        for (size_t j = 0; j < g.ny; ++j) {
            deadline.tick("checking the pieces");
            for (size_t i = 0; i < g.nx; ++i) {
                const size_t c = g.cell(i, j, k);
                if (!body.solid[c])
                    continue;
                ++count;
                const size_t neighbours[3] = { i > 0 ? g.cell(i - 1, j, k) : c, j > 0 ? g.cell(i, j - 1, k) : c,
                                               k > 0 ? g.cell(i, j, k - 1) : c };
                for (size_t other : neighbours) {
                    if (other == c || !body.solid[other])
                        continue;
                    const uint32_t a = find(uint32_t(c)), b = find(uint32_t(other));
                    if (a != b) {
                        parent[a] = b;
                        --count;
                    }
                }
            }
        }
    return count;
}

// The shortest run of solid cells through a cell along x, y or z, capped at 255, summarised: the share of solid cells
// whose shortest run is below `thin` cells, and whether the given cell is one of them.
struct ThinResult
{
    double fraction{ 0. };
    bool   at_cell{ false };
};

ThinResult thin_cells(const Body& body, size_t thin, size_t probe_cell, Deadline& deadline)
{
    const Grid&          g = body.grid;
    std::vector<uint8_t> shortest(g.cells(), 255);
    const size_t         dims[3]    = { g.nx, g.ny, g.nz };
    const size_t         strides[3] = { 1, g.nx, g.nx * g.ny };
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        for (size_t b = 0; b < dims[v]; ++b)
            for (size_t a = 0; a < dims[u]; ++a) {
                deadline.tick("checking wall thickness");
                const size_t base = a * strides[u] + b * strides[v];
                size_t       run  = 0;
                for (size_t t = 0; t <= dims[axis]; ++t) {
                    const bool in = t < dims[axis] && body.solid[base + t * strides[axis]];
                    if (in) {
                        ++run;
                        continue;
                    }
                    for (size_t r = 0; r < run; ++r) {
                        uint8_t& cell = shortest[base + (t - 1 - r) * strides[axis]];
                        cell          = uint8_t(std::min<size_t>({ cell, run, 255 }));
                    }
                    run = 0;
                }
            }
    }
    size_t solid = 0, few = 0;
    for (size_t c = 0; c < shortest.size(); ++c)
        if (body.solid[c]) {
            ++solid;
            few += shortest[c] < thin;
        }
    return { solid ? double(few) / double(solid) : 0., shortest[probe_cell] < thin };
}

} // namespace

static StressResult solve_stress_unguarded(const StressInput& input, const StressProgress& progress)
{
    const auto report = [&](int percent, const std::string& message) {
        if (progress)
            progress(percent, message);
    };

    // ---- input checks
    Deadline deadline;
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
    if (input.supports.size() > STRESS_MAX_REGIONS || input.loads.size() > STRESS_MAX_REGIONS)
        invalid("at most " + std::to_string(STRESS_MAX_REGIONS) + " supports and " + std::to_string(STRESS_MAX_REGIONS) + " loads are allowed");
    Vec3d force_sum = Vec3d::Zero(); // sum of the absolute components: no cancelling can hide an overflow
    for (const StressLoad& load : input.loads) {
        if (!load.force.allFinite())
            invalid("a load force must be finite");
        if ((load.force.array().abs() > STRESS_MAX_FORCE).any())
            invalid("a load force component exceeds " + Deadline::format_seconds(STRESS_MAX_FORCE) + " N");
        force_sum += load.force.cwiseAbs();
    }
    if ((force_sum.array() > STRESS_MAX_FORCE).any())
        invalid("the loads add up to more than " + Deadline::format_seconds(STRESS_MAX_FORCE) + " N in one direction");
    if (!(input.max_seconds > 0.) || !std::isfinite(input.max_seconds))
        invalid("max_seconds must be above 0");
    if (!(input.warning_seconds > 0.) || !std::isfinite(input.warning_seconds))
        invalid("warning_seconds must be finite and above 0");
    for (const StressSupport& s : input.supports)
        if (s.where.box && (!s.where.box->min.allFinite() || !s.where.box->max.allFinite()))
            invalid("a support box must be finite");
    for (const StressLoad& l : input.loads)
        if (l.where.box && (!l.where.box->min.allFinite() || !l.where.box->max.allFinite()))
            invalid("a load box must be finite");
    const size_t max_iterations = std::min(input.max_iterations, STRESS_MAX_ITERATIONS);
    const double max_seconds    = std::min(input.max_seconds, STRESS_MAX_SECONDS);
    const size_t max_selection  = std::min(input.max_selection_triangles, STRESS_MAX_SELECTION_TRIANGLES);
    const double byte_limit     = double(std::min(input.max_bytes, STRESS_MAX_BYTES));
    deadline.seconds            = max_seconds;

    // ---- the grid
    size_t triangle_total = 0;
    BoundingBoxf3 box;
    for (const indexed_triangle_set& part : input.parts) {
        triangle_total += part.indices.size();
        if (triangle_total > STRESS_MAX_TRIANGLES)
            invalid("the model has more than " + std::to_string(STRESS_MAX_TRIANGLES) + " triangles");
        for (const Vec3f& v : part.vertices) {
            deadline.tick("checking the input");
            if (!v.allFinite())
                invalid("the mesh has a vertex that is not finite (NaN or infinity)");
        }
        for (const auto& t : part.indices) {
            deadline.tick("checking the input");
            for (int c = 0; c < 3; ++c) {
                if (t[c] < 0 || size_t(t[c]) >= part.vertices.size())
                    invalid("the mesh has a triangle that refers to a vertex that does not exist");
                box.merge(part.vertices[t[c]].cast<double>());
            }
        }
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
    const double max_cells = double(std::min(input.max_cells, STRESS_MAX_CELLS));
    for (int d = 0; d < 3; ++d) {
        const double count = std::max(1., std::ceil(size[d] / grid.h - 1e-6));
        cells *= count;
        if (!(count <= max_cells) || !(cells <= max_cells)) // also catches sizes that would overflow size_t
            invalid("voxel_size is too small for this model: more than " + std::to_string(size_t(max_cells)) +
                    " cells (the limit), use a larger voxel_size");
        n[d] = size_t(count);
    }
    grid.nx = n[0];
    grid.ny = n[1];
    grid.nz = n[2];
    // The result is fetched layer by layer in chunks of at most STRESS_CHUNK_MAX_BYTES: one grid layer must fit.
    if (double(grid.nx) * double(grid.ny) * double(STRESS_CHUNK_BYTES_PER_CELL) > double(STRESS_CHUNK_MAX_BYTES))
        invalid("one grid layer has " + std::to_string(grid.nx * grid.ny) + " cells (" + std::to_string(grid.nx) + " x " + std::to_string(grid.ny) +
                "), more than one result chunk of " + std::to_string(STRESS_CHUNK_MAX_BYTES >> 20) + " MB holds (" +
                std::to_string(STRESS_CHUNK_BYTES_PER_CELL) + " bytes per cell); use a larger voxel_size");

    // ---- voxelization
    report(0, "Voxelizing");
    {
        // Memory before anything is built: the welding maps of the watertight check (one part at a time) and, later,
        // the surface crossings (16 bytes each, up to three times that while the vector grows) next to the solid grid.
        const double solid_bytes = cells;
        double       check_bytes = 0.;
        for (const indexed_triangle_set& part : input.parts)
            check_bytes = std::max(check_bytes, 100. * double(part.indices.size()) + 72. * double(part.vertices.size()));
        if (check_bytes + solid_bytes > byte_limit)
            invalid("checking the mesh would need about " + std::to_string(size_t((check_bytes + solid_bytes) / 1048576.)) +
                    " MB of memory, at most " + std::to_string(size_t(byte_limit / 1048576.)) + " MB are allowed; use a coarser model");
    }
    for (const indexed_triangle_set& part : input.parts)
        if (!is_watertight(part, deadline))
            failed("the mesh is not watertight (open or non-manifold edges)");
    b.solid.assign(grid.cells(), 0);
    {
        constexpr size_t MAX_HITS = 50000000;
        const double     room     = byte_limit - 49. * cells;
        const size_t     max_hits = room <= 0. ? 0 : std::min(MAX_HITS, size_t(room / 48.));
        for (const indexed_triangle_set& part : input.parts)
            voxelize(part, grid, b.solid, max_hits, deadline);
    }
    const size_t solid_cells = size_t(std::count(b.solid.begin(), b.solid.end(), 1));
    if (solid_cells == 0)
        failed("the voxel model is empty: the model is thinner than the voxel size");
    build_body(b, deadline);
    const size_t n_nodes = b.node_full.size(), n_dofs = 3 * n_nodes;
    double       memory_estimate = 0.;
    {
        // Bytes per solver node: node_full 4, adjacent 32, fixed 3, eight double dof vectors 192; per element:
        // elem_cell 4, elem_nodes 32, results 26; per cell: solid copy and result arrays 28; per grid node: active 4.
        memory_estimate = double(n_nodes) * 231. + double(b.elem_cell.size()) * 62. + double(grid.cells()) * 28. +
                          double(grid.node_count()) * 5.;
        if (memory_estimate > byte_limit)
            invalid("the analysis would need about " + std::to_string(size_t(memory_estimate / 1048576.)) + " MB of memory, at most " +
                    std::to_string(size_t(byte_limit / 1048576.)) + " MB are allowed; use a larger voxel_size");
    }

    // ---- supports and loads
    report(8, "Applying supports and loads");
    const TriangleSource source(input.parts);
    const size_t         max_quads = size_t(std::max(0., byte_limit - memory_estimate) / (2. * sizeof(Quad)));
    const auto pick = [&](const StressSelection& where, const char* what, size_t index) {
        std::vector<size_t> ids = where.triangles;
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end()); // a triangle listed twice counts once
        if (!ids.empty() && ids.back() >= source.count())
            invalid(std::string(what) + " " + std::to_string(index) + ": triangle " + std::to_string(ids.back()) + " does not exist");
        if (ids.size() > max_selection)
            invalid(std::string(what) + " " + std::to_string(index) + " selects " + std::to_string(ids.size()) + " triangles, at most " +
                    std::to_string(max_selection) + " are allowed");
        if (ids.empty() && !where.box)
            invalid(std::string(what) + " " + std::to_string(index) + " selects nothing (give triangles or a box)");
        Selected selected = select(b, where, ids, source, max_quads, deadline);
        if (std::none_of(selected.nodes.begin(), selected.nodes.end(), [](uint8_t v) { return v != 0; }))
            invalid(std::string(what) + " " + std::to_string(index) + " selects no material");
        return selected;
    };

    std::vector<uint8_t> fixed(n_dofs, 0);
    for (size_t s = 0; s < input.supports.size(); ++s) {
        const Selected selected = pick(input.supports[s].where, "support", s);
        for (size_t node = 0; node < n_nodes; ++node)
            if (selected.nodes[node])
                for (int a = 0; a < 3; ++a)
                    if (input.supports[s].axes[a])
                        fixed[3 * node + a] = 1;
    }
    std::vector<double> f(n_dofs, 0.);
    // What each load loses to the supports is judged on its own, against the sum of the magnitudes of all loads: loads
    // that cancel (a pair, a couple) neither hide a loss nor raise a false alarm.
    double              force_total = 0., force_lost = 0.;
    {
        std::vector<double> one(n_dofs, 0.);
        for (size_t l = 0; l < input.loads.size(); ++l) {
            std::fill(one.begin(), one.end(), 0.);
            distribute_load(pick(input.loads[l].where, "load", l), input.loads[l].force, one);
            Vec3d reaches = Vec3d::Zero();
            for (size_t i = 0; i < n_dofs; ++i) {
                if (!fixed[i])
                    reaches[i % 3] += one[i];
                f[i] += one[i];
            }
            force_total += input.loads[l].force.stableNorm();
            force_lost  += (input.loads[l].force - reaches).stableNorm();
        }
    }

    StressResult result;
    result.warnings   = input.warnings;
    result.size       = { grid.nx, grid.ny, grid.nz };
    result.voxel_size = grid.h;
    result.origin     = grid.origin;
    result.solid_cells = solid_cells;
    result.nodes      = n_nodes;
    result.fixed_dofs = size_t(std::count(fixed.begin(), fixed.end(), 1));
    double f_max = 0.;
    for (size_t i = 0; i < n_dofs; ++i)
        if (fixed[i])
            f[i] = 0.; // a load on a held node goes into the support
        else {
            result.applied_force[i % 3] += f[i];
            f_max = std::max(f_max, std::abs(f[i]));
        }
    // A load on a held node goes into the support and is lost for the body: say so when it is more than a hundredth.
    if (force_total > 0. && force_lost > 0.01 * force_total)
        result.warnings.push_back("Part of the load acts on nodes that a support holds and is taken by the support: " +
                                  Deadline::format_seconds(force_total - force_lost) + " N of " + Deadline::format_seconds(force_total) +
                                  " N reach the body. Keep loads and supports apart or use a smaller voxel_size.");
    if (!(f_max > 0.) || !std::isfinite(f_max))
        invalid("no load reaches a free node (all loaded nodes are held by a support, or the force is zero)");
    // The solver works on the load scaled to a largest component of 1 and the displacements are scaled back: the
    // problem is linear, and sums of squares can neither overflow (huge loads) nor underflow (tiny ones).
    for (double& value : f)
        value /= f_max;
    const double f_norm = std::sqrt(dot(f, f));

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
        for (size_t e = 0; e < b.elem_cell.size(); ++e) {
            deadline.tick("checking the pieces");
            for (int k = 1; k < 8; ++k)
                parent[find(b.elem_nodes[e * 8])] = find(b.elem_nodes[e * 8 + k]);
        }
        std::vector<uint8_t> held(n_nodes, 0);
        for (size_t node = 0; node < n_nodes; ++node)
            if (fixed[3 * node] || fixed[3 * node + 1] || fixed[3 * node + 2])
                held[find(uint32_t(node))] = 1;
        size_t pieces = 0;
        for (size_t node = 0; node < n_nodes; ++node)
            if (find(uint32_t(node)) == node) {
                ++pieces;
                if (!held[node])
                    failed("a separate piece of the model has no support (the voxel model falls into several pieces, one of them free)");
            }

        // The supports must hold the three translations and the three rotations of every piece. A rigid-body
        // mode m (a translation, or a rotation about an axis through a reference point) is a vector with one
        // value per dof. The supports hold all six exactly when these six vectors, looked at only on the held dofs,
        // are linearly independent: the 6 x 6 Gram matrix of the modes summed over the held dofs has full rank.
        // Each piece is judged on its own, relative to the centre of its held nodes and to their spread, so the
        // position of the piece and the size of the model do not matter.
        struct PieceHold
        {
            Vec3d  sum{ Vec3d::Zero() };
            size_t count{ 0 };
            double spread{ 0. };
            Mat6   gram{ Mat6::Zero() };
        };
        std::map<uint32_t, PieceHold> pieces_held;
        const auto position = [&](size_t node) {
            const auto ijk = node_ijk(grid, b.node_full[node]);
            return grid.node_position(ijk[0], ijk[1], ijk[2]);
        };
        for (size_t node = 0; node < n_nodes; ++node)
            if (fixed[3 * node] || fixed[3 * node + 1] || fixed[3 * node + 2]) {
                PieceHold& piece = pieces_held[find(uint32_t(node))];
                piece.sum += position(node);
                ++piece.count;
            }
        for (size_t node = 0; node < n_nodes; ++node)
            if (fixed[3 * node] || fixed[3 * node + 1] || fixed[3 * node + 2]) {
                PieceHold& piece = pieces_held[find(uint32_t(node))];
                piece.spread     = std::max(piece.spread, (position(node) - piece.sum / double(piece.count)).norm());
            }
        for (size_t node = 0; node < n_nodes; ++node) {
            deadline.tick("checking the supports");
            if (!(fixed[3 * node] || fixed[3 * node + 1] || fixed[3 * node + 2]))
                continue;
            PieceHold& piece = pieces_held[find(uint32_t(node))];
            const Vec3d p = piece.spread > 0. ? Vec3d((position(node) - piece.sum / double(piece.count)) / piece.spread) : Vec3d(Vec3d::Zero());
            for (int a = 0; a < 3; ++a) {
                if (!fixed[3 * node + a])
                    continue;
                Eigen::Matrix<double, 6, 1> m = Eigen::Matrix<double, 6, 1>::Zero();
                m[a] = 1.;
                for (int c = 0; c < 3; ++c) // rotation about axis c moves the point by e_c x p
                    m[3 + c] = Vec3d::Unit(c).cross(p)[a];
                piece.gram += m * m.transpose();
            }
        }
        for (const auto& [root, piece] : pieces_held) {
            const Eigen::SelfAdjointEigenSolver<Mat6> eigen(piece.gram, Eigen::EigenvaluesOnly);
            const auto&                               ev = eigen.eigenvalues();
            int                                       free_modes = 0;
            for (int i = 0; i < 6; ++i)
                free_modes += ev[i] <= 1e-9 * std::max(ev[5], 1e-300);
            if (free_modes > 0)
                failed("the supports do not hold the model against rigid-body motion: " + std::to_string(free_modes) +
                       " of 6 motions (3 translations, 3 rotations) stay free, so the load would move it without limit; "
                       "hold all three axes over a face, or enough points spread in all directions");
        }

        if (pieces < face_components(b, deadline))
            result.warnings.push_back("Parts of the model touch only at an edge or corner: such a joint carries no bending in the "
                                      "voxel model and acts like a hinge. Check the geometry.");
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
    // Stagnation: the best residual must fall by 1 % within every window of iterations, otherwise the system is
    // (nearly) singular or the tolerance is below what double precision can reach.
    constexpr size_t STAGNATION_WINDOW = 1000;
    double           best = r_norm, best_at_window_start = r_norm;
    bool             stalled = false, timed_out = false;
    while (result.iterations < max_iterations) {
        if (std::isfinite(r_norm) && r_norm <= input.tolerance * f_norm) {
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
        if (!std::isfinite(r_norm))
            break;
        best = std::min(best, r_norm);
        if (result.iterations % STAGNATION_WINDOW == 0) {
            if (best > 0.99 * best_at_window_start && best > input.tolerance * f_norm) {
                stalled = true;
                break;
            }
            best_at_window_start = best;
        }
        if (deadline.expired()) {
            timed_out = true;
            break;
        }
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
    // A residual that reached the tolerance in the very iteration in which the clock ran out is a result, not a timeout.
    if (!result.converged && std::isfinite(r_norm) && r_norm <= input.tolerance * f_norm)
        result.converged = true;
    if (timed_out && !result.converged)
        failed("the time limit of " + Deadline::format_seconds(max_seconds) + " s was reached while solving, after " +
               std::to_string(result.iterations) + " iterations (relative residual " + std::to_string(result.residual) +
               "); use a larger voxel_size or a looser tolerance");
    if (stalled && !result.converged)
        failed("the solver stagnated after " + std::to_string(result.iterations) + " iterations (relative residual " +
               std::to_string(result.residual) + "); the supports probably leave the body free to move or to rotate, or the tolerance is too tight");
    if (!result.converged)
        failed("the solver did not converge after " + std::to_string(result.iterations) + " iterations (relative residual " +
               std::to_string(result.residual) + "); the supports probably leave the body free to move or to rotate");

    // ---- results
    report(94, "Evaluating stress");
    for (double& value : u)
        value *= f_max; // back from the scaled load to the real one
    if (!std::all_of(u.begin(), u.end(), [](double v) { return std::isfinite(v); }))
        failed("the displacements overflow (material or load far outside any physical range)");
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
            result.von_mises[cell]     = float(std::min(vm, double(FLT_MAX)));
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
        const double len = Vec3d(u[3 * node], u[3 * node + 1], u[3 * node + 2]).stableNorm(); // no underflow for tiny loads
        if (len > result.max_displacement) {
            const uint32_t full = b.node_full[node];
            result.max_displacement    = len;
            result.max_displacement_at = grid.node_position(full % (grid.nx + 1), (full / (grid.nx + 1)) % (grid.ny + 1),
                                                            full / ((grid.nx + 1) * (grid.ny + 1)));
        }
    }

    // ---- warnings
    {
        const auto index_of = [&](int d, size_t count) {
            return size_t(std::clamp(std::floor((result.max_von_mises_at[d] - grid.origin[d]) / grid.h), 0., double(count) - 1.));
        };
        const size_t max_cell = grid.cell(index_of(0, grid.nx), index_of(1, grid.ny), index_of(2, grid.nz));
        // The converged result is never thrown away for the warnings: they have a time budget of their own.
        Deadline warning_clock;
        // Within max_seconds, which counts from the start of the call: the check gets what is left of it, at most warning_seconds.
        warning_clock.seconds = std::max(0., std::min(input.warning_seconds, max_seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - deadline.start).count()));
        ThinResult thin;
        bool       thin_checked = true;
        try {
            thin = thin_cells(b, 4, max_cell, warning_clock);
        } catch (const EngineError&) {
            thin_checked = false;
            result.warnings.push_back("The check for thin walls was skipped (time limit); the result itself is complete.");
        }
        if (thin_checked && (thin.fraction >= 0.05 || thin.at_cell))
            result.warnings.push_back("Walls or ribs are thinner than 4 cells" + std::string(thin.at_cell ? ", also where the stress is highest" : "") +
                                      " (" + std::to_string(int(std::lround(100. * thin.fraction))) +
                                      " % of the material): the elements lock in bending and the result is too stiff and too low in stress. "
                                      "Use a smaller voxel_size.");
        double mesh_volume = 0.;
        for (const indexed_triangle_set& part : input.parts)
            mesh_volume += std::abs(double(its_volume(part)));
        const double voxel_volume = double(solid_cells) * grid.h * grid.h * grid.h;
        if (mesh_volume > 0. && std::abs(voxel_volume - mesh_volume) > 0.2 * mesh_volume)
            result.warnings.push_back("The voxel model has a volume of " + std::to_string(int(std::lround(100. * voxel_volume / mesh_volume))) +
                                      " % of the mesh volume: thin parts are lost or overlapping parts counted twice. Use a smaller voxel_size.");
    }
    report(100, "Finished");
    return result;
}

StressResult solve_stress(const StressInput& input, const StressProgress& progress)
{
    try {
        return solve_stress_unguarded(input, progress);
    } catch (const std::bad_alloc&) {
        throw EngineError(ErrorCode::EngineError, "not enough memory for this analysis; use a larger voxel_size");
    } catch (const std::length_error&) {
        throw EngineError(ErrorCode::EngineError, "the analysis is too large for the available memory; use a larger voxel_size");
    }
}

} // namespace Slic3r::engine
