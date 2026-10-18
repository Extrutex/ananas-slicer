#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../TexturePainting.hpp"
#include "ResourcePathUtils.hpp"

#include "OBJ.hpp"
#include "objparser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

//Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

namespace {

// A face that is not convex is ear clipped, at a cost of its corners times its reflex corners. Beyond
// this many corners such a face is left out rather than stalling the import.
constexpr size_t MAX_EAR_CLIPPED_FACE_CORNERS = 20000;

enum class FaceSplit { Ok, Degenerate, SelfIntersecting, TooManyCorners };

double cross2(const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); }

int orientation(const Vec2d &a, const Vec2d &b, const Vec2d &c)
{
    const double v = cross2(b - a, c - a);
    return (v > 0.) - (v < 0.);
}

// Whether q, collinear with segment ab, lies on it.
bool on_segment(const Vec2d &a, const Vec2d &b, const Vec2d &q)
{
    return q.x() >= std::min(a.x(), b.x()) && q.x() <= std::max(a.x(), b.x()) && q.y() >= std::min(a.y(), b.y()) &&
           q.y() <= std::max(a.y(), b.y());
}

// Whether the closed segments ab and cd share a point.
bool segments_touch(const Vec2d &a, const Vec2d &b, const Vec2d &c, const Vec2d &d)
{
    const int o1 = orientation(a, b, c), o2 = orientation(a, b, d), o3 = orientation(c, d, a), o4 = orientation(c, d, b);
    if (o1 != o2 && o3 != o4)
        return true;
    return (o1 == 0 && on_segment(a, b, c)) || (o2 == 0 && on_segment(a, b, d)) || (o3 == 0 && on_segment(c, d, a)) ||
           (o4 == 0 && on_segment(c, d, b));
}

// Whether the closed outline crosses or touches itself, or runs straight back along an edge.
bool outline_self_intersects(const std::vector<Vec2d> &p)
{
    const int n = int(p.size());
    for (int i = 0; i < n; ++i) {
        const Vec2d &a = p[(i + n - 1) % n], &b = p[i], &c = p[(i + 1) % n];
        if (orientation(a, b, c) == 0 && (b - a).dot(c - b) < 0.)
            return true;
    }
    // Edge i runs from corner i to corner i + 1. Sorted by their left end, an edge only has to be
    // compared with the edges that start before its right end.
    auto min_x = [&p, n](int i) { return std::min(p[i].x(), p[(i + 1) % n].x()); };
    auto max_x = [&p, n](int i) { return std::max(p[i].x(), p[(i + 1) % n].x()); };
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&min_x](int l, int r) { return min_x(l) < min_x(r); });
    for (int k = 0; k < n; ++k) {
        const int    i   = order[k];
        const double end = max_x(i);
        for (int l = k + 1; l < n && min_x(order[l]) <= end; ++l) {
            const int j = order[l];
            // Neighbouring edges share a corner, which the check above covers.
            if (j == (i + 1) % n || i == (j + 1) % n)
                continue;
            if (segments_touch(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n]))
                return true;
        }
    }
    return false;
}

// Splits one OBJ face into triangles, given as positions into `corners` and wound like the face.
// A triangle is kept as it is. A larger face is split in its best-fit plane, after dropping corners that
// repeat the one before them: a strictly convex face becomes a fan from its first corner, which for a
// quad is the first-to-third corner diagonal the loader has always used, and any other face with a
// simple outline is ear clipped, so its triangles stay inside the outline. A face without area or with
// an outline that crosses or touches itself has no such split and is rejected, as is a non-convex face
// with more than MAX_EAR_CLIPPED_FACE_CORNERS corners; `triangles` is then empty.
FaceSplit triangulate_face(const std::vector<Vec3f> &corners, std::vector<std::array<int, 3>> &triangles)
{
    triangles.clear();
    if (corners.size() == 3) {
        triangles.push_back({0, 1, 2});
        return FaceSplit::Ok;
    }

    std::vector<int> ids;
    ids.reserve(corners.size());
    for (int i = 0; i < int(corners.size()); ++i)
        if (ids.empty() || corners[i] != corners[ids.back()])
            ids.push_back(i);
    while (ids.size() > 1 && corners[ids.back()] == corners[ids.front()])
        ids.pop_back();
    const int n = int(ids.size());
    if (n < 3)
        return FaceSplit::Degenerate;

    // Newell's normal: the face winds counter-clockwise around it, also when it is not quite planar.
    // Its length is twice the area of the face.
    Vec3d normal = Vec3d::Zero();
    Vec3d lo     = corners[ids.front()].cast<double>();
    Vec3d hi     = lo;
    for (int i = 0; i < n; ++i) {
        const Vec3d a = corners[ids[i]].cast<double>();
        const Vec3d b = corners[ids[(i + 1) % n]].cast<double>();
        normal += Vec3d((a.y() - b.y()) * (a.z() + b.z()), (a.z() - b.z()) * (a.x() + b.x()), (a.x() - b.x()) * (a.y() + b.y()));
        lo = lo.cwiseMin(a);
        hi = hi.cwiseMax(a);
    }
    // A face whose corners lie on a line, or whose outline encloses as much area clockwise as
    // counter-clockwise like a bow-tie, has no side to cover.
    if (!(normal.norm() > 1e-12 * (hi - lo).squaredNorm()))
        return FaceSplit::Degenerate;
    if (n == 3) {
        triangles.push_back({ids[0], ids[1], ids[2]});
        return FaceSplit::Ok;
    }
    normal.normalize();
    // u x v = normal, so the face is counter-clockwise in (u, v) as well.
    const Vec3d u = (std::abs(normal.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).cross(normal).normalized();
    const Vec3d v = normal.cross(u);
    std::vector<Vec2d> p(n);
    for (int i = 0; i < n; ++i) {
        const Vec3d d = (corners[ids[i]] - corners[ids.front()]).cast<double>();
        p[i]          = Vec2d(d.dot(u), d.dot(v));
    }

    // Left turns only, adding up to a single full turn, make a convex outline; a star polygon turns
    // left only as well, but twice around.
    bool   strictly_convex = true;
    double turning         = 0.;
    for (int i = 0; i < n && strictly_convex; ++i) {
        const Vec2d  e0 = p[i] - p[(i + n - 1) % n];
        const Vec2d  e1 = p[(i + 1) % n] - p[i];
        const double c  = cross2(e0, e1);
        strictly_convex = c > 0.;
        turning += std::atan2(c, e0.dot(e1));
    }
    if (strictly_convex && turning < 3. * M_PI) {
        for (int k = 1; k + 1 < n; ++k)
            triangles.push_back({ids[0], ids[k], ids[k + 1]});
        return FaceSplit::Ok;
    }
    if (size_t(n) > MAX_EAR_CLIPPED_FACE_CORNERS)
        return FaceSplit::TooManyCorners;
    if (outline_self_intersects(p))
        return FaceSplit::SelfIntersecting;

    // Ear clipping as in Eberly, "Triangulation by Ear Clipping". A corner is an ear when it is strictly
    // convex and its triangle holds no other corner; in a simple outline it is enough to look for the
    // reflex and straight ones. Clipping an ear changes only the triangles of its two neighbours, and a
    // corner only ever turns from reflex to convex, so the ear flags of all other corners stay valid.
    std::vector<int> prev(n), next(n);
    for (int i = 0; i < n; ++i) {
        prev[i] = (i + n - 1) % n;
        next[i] = (i + 1) % n;
    }
    auto cross = [&p](int a, int b, int c) { return cross2(p[b] - p[a], p[c] - p[a]); };
    // The reflex and straight corners, and where each one sits in that list.
    std::vector<int> reflex;
    std::vector<int> reflex_slot(n, -1);
    for (int i = 0; i < n; ++i)
        if (cross(prev[i], i, next[i]) <= 0.) {
            reflex_slot[i] = int(reflex.size());
            reflex.push_back(i);
        }
    auto drop_reflex = [&reflex, &reflex_slot](int i) {
        const int slot    = reflex_slot[i];
        const int last    = reflex.back();
        reflex[slot]      = last;
        reflex_slot[last] = slot;
        reflex.pop_back();
        reflex_slot[i] = -1;
    };
    auto is_ear = [&](int i) {
        if (reflex_slot[i] >= 0)
            return false;
        const int a = prev[i], c = next[i];
        for (int q : reflex)
            if (q != a && q != c && cross(a, i, q) >= 0. && cross(i, c, q) >= 0. && cross(c, a, q) >= 0.)
                return false;
        return true;
    };
    std::vector<char> ear(n);
    std::vector<int>  ears;
    for (int i = n - 1; i >= 0; --i)
        if ((ear[i] = is_ear(i)))
            ears.push_back(i);

    triangles.reserve(n - 2);
    int last = 0;
    for (int remaining = n; remaining > 3; --remaining) {
        int i = -1;
        while (!ears.empty() && i < 0) {
            i = ears.back();
            ears.pop_back();
            if (!ear[i])
                i = -1;
        }
        // Only rounding in a nearly degenerate outline can leave a simple polygon without ears.
        if (i < 0) {
            triangles.clear();
            return FaceSplit::Degenerate;
        }
        const int a = prev[i], c = next[i];
        triangles.push_back({ids[a], ids[i], ids[c]});
        ear[i]  = false;
        next[a] = c;
        prev[c] = a;
        for (int k : {a, c}) {
            if (reflex_slot[k] >= 0 && cross(prev[k], k, next[k]) > 0.)
                drop_reflex(k);
            if ((ear[k] = is_ear(k)))
                ears.push_back(k);
        }
        last = c;
    }
    triangles.push_back({ids[prev[last]], ids[last], ids[next[last]]});
    return FaceSplit::Ok;
}

} // namespace

bool load_obj(const char *path, TriangleMesh *meshptr, ObjInfo& obj_info, std::string &message, ObjParser::MtlData *out_mtl)
{
    if (meshptr == nullptr)
        return false;
    // Parse the OBJ file.
    ObjParser::ObjData data;
    ObjParser::MtlData mtl_data;
    if (! ObjParser::objparse(path, data)) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path;
        message = _L("load_obj: failed to parse");
        return false;
    }
    bool exist_mtl = false;
    if (data.mtllibs.size() > 0) { // read mtl
        for (auto mtl_name : data.mtllibs) {
            if (mtl_name.size() == 0){
                continue;
            }
            exist_mtl = true;
            bool                    mtl_name_is_path = false;
            boost::filesystem::path mtl_abs_path(mtl_name);
            if (boost::filesystem::exists(mtl_abs_path)) {
                mtl_name_is_path = true;
            }
            boost::filesystem::path mtl_path;
            if (!mtl_name_is_path) {
                boost::filesystem::path full_path(path);
                std::string             dir = full_path.parent_path().string();
                auto                    mtl_file = dir + "/" + mtl_name;
                boost::filesystem::path temp_mtl_path(mtl_file);
                mtl_path = temp_mtl_path;
            }
            const std::string _mtl_path = (mtl_name_is_path ? mtl_abs_path : mtl_path).string();
            if (boost::filesystem::exists(mtl_name_is_path ? mtl_abs_path : mtl_path)) {
                if (!ObjParser::mtlparse(_mtl_path.c_str(), mtl_data)) {
                    BOOST_LOG_TRIVIAL(error) << "load_obj:load_mtl: failed to parse " << _mtl_path;
                    message = _L("load mtl in obj: failed to parse");
                    return false;
                }
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to load mtl_path:" << _mtl_path;
            }
        }
    }
    // Count the triangles the faces split into, and verify that every face has at least three corners.
    size_t num_triangles = 0;
    for (size_t i = 0; i < data.vertices.size(); ++ i) {
        // Find the end of face.
        size_t j = i;
        for (; j < data.vertices.size() && data.vertices[j].coordIdx != -1; ++ j) ;
        if (size_t num_face_vertices = j - i; num_face_vertices > 0) {
            if (num_face_vertices < 3) {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with less than 2 vertices.";
                message = _L("The file contains polygons with less than 2 vertices.");
                return false;
            }
            // A face with n corners splits into n - 2 triangles, which is also how the parser
            // counts the faces of a material.
            num_triangles += num_face_vertices - 2;
            i = j;
        }
    }
    // Convert ObjData into indexed triangle set.
    indexed_triangle_set its;
    size_t               num_vertices = data.coordinates.size() / OBJ_VERTEX_LENGTH;
    its.vertices.reserve(num_vertices);
    its.indices.reserve(num_triangles);
    if (exist_mtl) {
        obj_info.is_single_mtl = data.usemtls.size() == 1 && mtl_data.new_mtl_unmap.size() == 1;
        obj_info.usemtls = data.usemtls;
        obj_info.face_colors.reserve(num_triangles);
    }
    bool has_color = data.has_vertex_color;
    for (size_t i = 0; i < num_vertices; ++ i) {
        size_t j = i * OBJ_VERTEX_LENGTH;
        its.vertices.emplace_back(data.coordinates[j], data.coordinates[j + 1], data.coordinates[j + 2]);
        if (data.has_vertex_color) {
            RGBA color{std::clamp(data.coordinates[j + 3], 0.f, 1.f), std::clamp(data.coordinates[j + 4], 0.f, 1.f), std::clamp(data.coordinates[j + 5], 0.f, 1.f),
                       std::clamp(data.coordinates[j + 6], 0.f, 1.f)};
            obj_info.vertex_colors.emplace_back(color);
        }
    }
    std::vector<int>   indices;
    std::vector<int>   uvs;
    std::vector<Vec3f> corners;
    std::vector<std::array<int, 3>> triangles;
    // Triangles the faces read so far count for in the material spans.
    size_t mtl_face_index              = 0;
    size_t num_degenerate_faces        = 0;
    size_t num_self_intersecting_faces = 0;
    size_t num_oversized_faces         = 0;
    for (size_t i = 0; i < data.vertices.size();)
        if (data.vertices[i].coordIdx == -1)
            ++ i;
        else {
            indices.clear();
            uvs.clear();
            while (i < data.vertices.size())
                if (const ObjParser::ObjVertex &vertex = data.vertices[i ++]; vertex.coordIdx == -1) {
                    break;
                } else {
                    if (vertex.coordIdx < 0 || vertex.coordIdx >= int(its.vertices.size())) {
                        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains invalid vertex index.";
                        message = _L("The file contains invalid vertex index.");
                        return false;
                    }
                    indices.push_back(vertex.coordIdx);
                    uvs.push_back(vertex.textureCoordIdx);
                }
            if (! indices.empty()) {
                assert(indices.size() >= 3);
                RGBA face_color;
                auto set_face_color = [&uvs, &data, &mtl_data, &obj_info, &face_color](int face_index, const std::string mtl_name, const std::array<int, 3> &corners) {
                    if (mtl_data.new_mtl_unmap.find(mtl_name) != mtl_data.new_mtl_unmap.end()) {
                        bool is_merge_ka_kd = true;
                        for (size_t n = 0; n < 3; n++) {
                            if (float(mtl_data.new_mtl_unmap[mtl_name]->Ka[n] + mtl_data.new_mtl_unmap[mtl_name]->Kd[n]) > 1.0) {
                                is_merge_ka_kd=false;
                                break;
                            }
                        }
                        for (size_t n = 0; n < 3; n++) {
                            if (is_merge_ka_kd) {
                                face_color[n] = std::clamp(float(mtl_data.new_mtl_unmap[mtl_name]->Ka[n] + mtl_data.new_mtl_unmap[mtl_name]->Kd[n]), 0.f, 1.f);
                            }
                            else {
                                face_color[n] = std::clamp(float(mtl_data.new_mtl_unmap[mtl_name]->Kd[n]), 0.f, 1.f);
                            }
                        }
                        face_color[3] = mtl_data.new_mtl_unmap[mtl_name]->Tr; // alpha
                        if (mtl_data.new_mtl_unmap[mtl_name]->map_Kd.size() > 0) {
                            auto png_name       = mtl_data.new_mtl_unmap[mtl_name]->map_Kd;
                            obj_info.has_uv_png = true;
                            if (obj_info.pngs.find(png_name) == obj_info.pngs.end()) { obj_info.pngs[png_name] = false; }
                            obj_info.uv_map_pngs[face_index] = png_name;
                        }
                        if (data.textureCoordinates.size() > 0) {
                            // A face vertex may omit vt or reference a missing one. Fall back to (0, 0) rather than
                            // skipping the face, so obj_info.uvs stays aligned with the face indices.
                            const int uv_count = static_cast<int>(data.textureCoordinates.size() / OBJ_TEXCOORD_LENGTH);
                            auto      uv_at    = [&data, uv_count](int idx) -> Vec2f {
                                if (idx < 0 || idx >= uv_count)
                                    return Vec2f::Zero();
                                return Vec2f(data.textureCoordinates[idx * OBJ_TEXCOORD_LENGTH], data.textureCoordinates[idx * OBJ_TEXCOORD_LENGTH + 1]);
                            };
                            std::array<Vec2f, 3> uv_array{uv_at(uvs[corners[0]]), uv_at(uvs[corners[1]]), uv_at(uvs[corners[2]])};
                            obj_info.uvs.emplace_back(uv_array);
                        }
                        obj_info.face_colors.emplace_back(face_color);
                    }
                    else {
                        if (obj_info.lost_material_name.empty()) {
                            obj_info.lost_material_name = mtl_name;
                        }
                    }
                };
                // The material spans count n - 2 triangles for a face of n corners, also for a face that
                // is rejected or splits into fewer, so they are looked up by that count.
                auto set_face_color_by_mtl = [&data, &set_face_color](int face_index, int span_index, const std::array<int, 3> &corners) {
                    if (data.usemtls.size() == 1) {
                        set_face_color(face_index, data.usemtls[0].name, corners);
                    } else {
                        for (size_t k = 0; k < data.usemtls.size(); k++) {
                            auto mtl = data.usemtls[k];
                            if (span_index >= mtl.face_start && span_index <= mtl.face_end) {
                                set_face_color(face_index, data.usemtls[k].name, corners);
                                break;
                            }
                        }
                    }
                };
                corners.clear();
                for (int index : indices)
                    corners.push_back(its.vertices[index]);
                switch (triangulate_face(corners, triangles)) {
                case FaceSplit::Ok: break;
                case FaceSplit::Degenerate: ++num_degenerate_faces; break;
                case FaceSplit::SelfIntersecting: ++num_self_intersecting_faces; break;
                case FaceSplit::TooManyCorners: ++num_oversized_faces; break;
                }
                for (size_t k = 0; k < triangles.size(); ++k) {
                    const std::array<int, 3> &triangle = triangles[k];
                    its.indices.emplace_back(indices[triangle[0]], indices[triangle[1]], indices[triangle[2]]);
                    if (exist_mtl)
                        set_face_color_by_mtl(int(its.indices.size()) - 1, int(mtl_face_index + k), triangle);
                }
                mtl_face_index += indices.size() - 2;
            }
        }

    obj_info.rejected_faces = num_degenerate_faces + num_self_intersecting_faces + num_oversized_faces;
    if (obj_info.rejected_faces > 0)
        BOOST_LOG_TRIVIAL(warning) << "load_obj: " << path << ": left out " << obj_info.rejected_faces
                                   << " faces that cannot be split into triangles: " << num_degenerate_faces << " without area, "
                                   << num_self_intersecting_faces << " with a self-intersecting outline, " << num_oversized_faces
                                   << " not convex and with more than " << MAX_EAR_CLIPPED_FACE_CORNERS << " corners.";

    *meshptr = TriangleMesh(std::move(its));
    if (meshptr->empty()) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: This OBJ file couldn't be read because it's empty. " << path;
        message = _L("This OBJ file couldn't be read because it's empty.");
        return false;
    }
    if (meshptr->volume() < 0) {
        meshptr->flip_triangles();
        // Flipping swaps corners 1 and 2 of every face, so the UVs have to follow.
        for (std::array<Vec2f, 3> &uv : obj_info.uvs)
            std::swap(uv[1], uv[2]);
    }
    // Hand the parsed material table back so callers can build a TexturedMesh from it.
    if (out_mtl)
        *out_mtl = mtl_data;
    return true;
}

bool load_obj(const char *path, Model *model, ObjInfo& obj_info, std::string &message, const char *object_name_in, ObjParser::MtlData *out_mtl)
{
    TriangleMesh mesh;

    bool ret = load_obj(path, &mesh, obj_info, message, out_mtl);

    if (ret) {
        std::string  object_name;
        if (object_name_in == nullptr) {
            const char *last_slash = strrchr(path, DIR_SEPARATOR);
            object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
        } else
           object_name.assign(object_name_in);
        model->add_object(object_name.c_str(), path, std::move(mesh));
    }

    return ret;
}

bool obj_to_textured_mesh(
    const ObjInfo& obj_info,
    const indexed_triangle_set& its,
    const ObjParser::MtlData& mtl_data,
    const std::string& obj_directory,
    TexturedMesh& out)
{
    if (its.vertices.empty() || its.indices.empty() || !obj_info.has_uv_png)
        return false;

    const size_t nv = its.vertices.size();
    const size_t nf = its.indices.size();

    // 1. Copy vertices
    out.vertices.resize(nv);
    for (size_t i = 0; i < nv; ++i)
        out.vertices[i] = {its.vertices[i].x(), its.vertices[i].y(), its.vertices[i].z()};

    // 2. Copy face indices
    out.indices.resize(nf);
    for (size_t i = 0; i < nf; ++i)
        out.indices[i] = {its.indices[i][0], its.indices[i][1], its.indices[i][2]};

    // 3. Build per-face UV (uv_coords + uv_indices)
    // OBJ UV convention: V=0 at bottom (OpenGL); texture sampling expects V=0 at top (like glTF/OpenCV).
    // Flip V here so downstream code works uniformly.
    if (!obj_info.uvs.empty()) {
        const size_t uv_face_count = obj_info.uvs.size();
        out.uv_coords.resize(uv_face_count * 3);
        out.uv_indices.resize(nf);
        for (size_t fi = 0; fi < nf; ++fi) {
            if (fi < uv_face_count) {
                int base = static_cast<int>(fi * 3);
                out.uv_coords[base + 0] = {obj_info.uvs[fi][0].x(), 1.f - obj_info.uvs[fi][0].y()};
                out.uv_coords[base + 1] = {obj_info.uvs[fi][1].x(), 1.f - obj_info.uvs[fi][1].y()};
                out.uv_coords[base + 2] = {obj_info.uvs[fi][2].x(), 1.f - obj_info.uvs[fi][2].y()};
                out.uv_indices[fi] = {base, base + 1, base + 2};
            } else {
                out.uv_indices[fi] = {0, 0, 0};
            }
        }
    }

    // 4. Build material list and load textures from disk
    // Map: material name -> material index
    std::map<std::string, int> mtl_name_to_idx;
    for (size_t i = 0; i < mtl_data.mtl_orders.size(); ++i)
        mtl_name_to_idx[mtl_data.mtl_orders[i]] = static_cast<int>(i);

    const int num_materials = static_cast<int>(mtl_data.mtl_orders.size());
    out.material_colors.resize(num_materials, {1.f, 1.f, 1.f, 1.f});
    out.material_texture_map.resize(num_materials, -1);

    // Map: texture filename -> index in out.textures
    std::map<std::string, int> png_to_tex_idx;

    for (int mi = 0; mi < num_materials; ++mi) {
        const std::string& name = mtl_data.mtl_orders[mi];
        auto it = mtl_data.new_mtl_unmap.find(name);
        if (it == mtl_data.new_mtl_unmap.end())
            continue;
        const auto& mtl = *(it->second);

        // Material color from Kd
        out.material_colors[mi] = {mtl.Kd[0], mtl.Kd[1], mtl.Kd[2], mtl.Tr};

        // Texture from map_Kd
        if (mtl.map_Kd.empty())
            continue;

        auto tex_it = png_to_tex_idx.find(mtl.map_Kd);
        if (tex_it != png_to_tex_idx.end()) {
            out.material_texture_map[mi] = tex_it->second;
            continue;
        }

        // Resolve texture file path.
        const boost::filesystem::path requested_tex_path(mtl.map_Kd);
        const boost::filesystem::path tex_path = requested_tex_path.is_absolute() ?
            resource_path::resolve_existing_path_case_insensitive(requested_tex_path, "obj_to_textured_mesh: map_Kd") :
            resource_path::resolve_existing_relative_path_case_insensitive(
                boost::filesystem::path(obj_directory), requested_tex_path, "obj_to_textured_mesh: map_Kd");

        if (tex_path.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "obj_to_textured_mesh: texture not found: " << requested_tex_path;
            continue;
        }

        // Read raw file bytes
        boost::nowide::ifstream file(tex_path.string(), std::ios::binary | std::ios::ate);
        if (!file.is_open())
            continue;
        auto file_size = file.tellg();
        if (file_size <= 0)
            continue;
        file.seekg(0, std::ios::beg);

        TextureImage ti;
        ti.data.resize(static_cast<size_t>(file_size));
        file.read(reinterpret_cast<char*>(ti.data.data()), file_size);
        ti.width = -1;
        ti.height = -1;
        ti.channels = 0;

        int new_idx = static_cast<int>(out.textures.size());
        out.textures.push_back(std::move(ti));
        png_to_tex_idx[mtl.map_Kd] = new_idx;
        out.material_texture_map[mi] = new_idx;
    }

    // 5. Build per-face material_ids from usemtls ranges
    out.material_ids.resize(nf, -1);
    if (!obj_info.usemtls.empty()) {
        for (size_t fi = 0; fi < nf; ++fi) {
            int face_idx = static_cast<int>(fi);
            for (size_t k = 0; k < obj_info.usemtls.size(); ++k) {
                const auto& um = obj_info.usemtls[k];
                if (face_idx >= um.face_start && face_idx <= um.face_end) {
                    auto name_it = mtl_name_to_idx.find(um.name);
                    if (name_it != mtl_name_to_idx.end())
                        out.material_ids[fi] = name_it->second;
                    break;
                }
            }
        }
    }

    if (out.textures.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "obj_to_textured_mesh: no textures loaded";
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "obj_to_textured_mesh: " << nf << " faces, "
                            << out.textures.size() << " textures, "
                            << num_materials << " materials";
    return true;
}

bool store_obj(const char *path, TriangleMesh *mesh)
{
    //FIXME returning false even if write failed.
    mesh->WriteOBJFile(path);
    return true;
}

bool store_obj(const char *path, ModelObject *model_object)
{
    TriangleMesh mesh = model_object->mesh();
    return store_obj(path, &mesh);
}

bool store_obj(const char *path, Model *model)
{
    TriangleMesh mesh = model->mesh();
    return store_obj(path, &mesh);
}

}; // namespace Slic3r
