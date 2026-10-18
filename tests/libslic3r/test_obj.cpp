#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/TexturePainting.hpp"

#include <boost/nowide/fstream.hpp>

#include <chrono>
#include <cmath>

#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

struct LoadedObj
{
    bool         ok{false};
    TriangleMesh mesh;
    ObjInfo      info;
};

// Loads an OBJ made of the given lines, with a material library that defines material "a".
LoadedObj load_textured_obj(const std::string &body)
{
    ScopedTemporaryFile obj(".obj");
    ScopedTemporaryFile mtl(".mtl");
    {
        boost::nowide::ofstream out(mtl.string());
        out << "newmtl a\nKd 1 0 0\n";
    }
    {
        boost::nowide::ofstream out(obj.string());
        out << "mtllib " << mtl.path().filename().string() << "\n" << body;
    }
    LoadedObj   loaded;
    std::string message;
    loaded.ok = load_obj(obj.string().c_str(), &loaded.mesh, loaded.info, message);
    return loaded;
}

// Loads an OBJ made of the given lines, without any material.
LoadedObj load_plain_obj(const std::string &body)
{
    ScopedTemporaryFile obj(".obj");
    {
        boost::nowide::ofstream out(obj.string());
        out << body;
    }
    LoadedObj   loaded;
    std::string message;
    loaded.ok = load_obj(obj.string().c_str(), &loaded.mesh, loaded.info, message);
    return loaded;
}

// Loads a single face lying in the z = 0 plane, its corners given counter-clockwise from above.
LoadedObj load_flat_face(const std::vector<Vec2f> &corners)
{
    std::string body;
    for (const Vec2f &c : corners)
        body += "v " + std::to_string(c.x()) + " " + std::to_string(c.y()) + " 0\n";
    body += "f";
    for (size_t i = 1; i <= corners.size(); ++i)
        body += " " + std::to_string(i);
    return load_plain_obj(body + "\n");
}

// A flat face is covered exactly when no triangle faces down and the triangle areas add up to the
// face area: a triangle outside the outline is either flipped or makes the sum too large.
void check_covers_flat_face(const indexed_triangle_set &its, double face_area, double tolerance = 1e-3)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices &face : its.indices) {
        const Vec3f a = its.vertices[face[0]], b = its.vertices[face[1]], c = its.vertices[face[2]];
        const Vec3f n = (b - a).cross(c - a);
        CAPTURE(a, b, c);
        CHECK(n.z() > 0.f);
        area += 0.5 * n.norm();
    }
    CHECK_THAT(area, WithinAbs(face_area, tolerance));
}

// A tetrahedron with a material and two texture coordinates, (0.25, 0.5) and (0.75, 1).
// Only the first face and the vt lines are varied; the other three faces reference vt 1.
LoadedObj load_textured_tetrahedron(const std::string &first_face, const std::string &vts = "vt 0.25 0.5\nvt 0.75 1\n")
{
    return load_textured_obj("v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n" + vts + "usemtl a\n" + first_face + "\n" +
                             "f 1/1 2/1 4/1\nf 1/1 4/1 3/1\nf 2/1 3/1 4/1\n");
}

// Texture coordinate n is (n / 10, n / 20), so a UV identifies the vt it came from.
void check_uv_is_vt(const Vec2f &uv, int vt)
{
    CHECK_THAT(uv.x(), WithinAbs(vt / 10., 1e-6));
    CHECK_THAT(uv.y(), WithinAbs(vt / 20., 1e-6));
}

// The corners of a star in the z = 0 plane, counter-clockwise, alternating between radius 100 and 50.
std::vector<Vec2f> star_corners(int num_corners)
{
    std::vector<Vec2f> corners;
    for (int i = 0; i < num_corners; ++i) {
        const double r = i % 2 == 0 ? 100. : 50.;
        corners.emplace_back(float(r * std::cos(2. * M_PI * i / num_corners)), float(r * std::sin(2. * M_PI * i / num_corners)));
    }
    return corners;
}

// The area of a polygon given by its corners, positive when they run counter-clockwise.
double signed_area(const std::vector<Vec2f> &corners)
{
    double area = 0.;
    for (size_t i = 0; i < corners.size(); ++i) {
        const Vec2d a = corners[i].cast<double>(), b = corners[(i + 1) % corners.size()].cast<double>();
        area += 0.5 * (a.x() * b.y() - a.y() * b.x());
    }
    return area;
}

// Loads a triangle followed by a second face, so the file still has a mesh when that face is left out.
LoadedObj load_triangle_and_face(const std::vector<Vec2f> &corners)
{
    std::string body = "v 0 0 -10\nv 10 0 -10\nv 0 10 -10\n";
    for (const Vec2f &c : corners)
        body += "v " + std::to_string(c.x()) + " " + std::to_string(c.y()) + " 0\n";
    body += "f 1 2 3\nf";
    for (size_t i = 1; i <= corners.size(); ++i)
        body += " " + std::to_string(i + 3);
    return load_plain_obj(body + "\n");
}

} // namespace

TEST_CASE("An out-of-range texture index falls back to a zero UV and keeps the geometry", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1/1000000000 3/1 2/1");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0., 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0., 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(0.5, 1e-6));
}

TEST_CASE("A face without texture indices loads among faces that have them", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1 3 2");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    // One UV entry per face, so later faces keep their own coordinates.
    REQUIRE(loaded.info.uvs.size() == 4);
    for (const Vec2f &uv : loaded.info.uvs.front()) {
        CHECK_THAT(uv.x(), WithinAbs(0., 1e-6));
        CHECK_THAT(uv.y(), WithinAbs(0., 1e-6));
    }
    CHECK_THAT(loaded.info.uvs[1][0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(loaded.info.uvs[1][0].y(), WithinAbs(0.5, 1e-6));
}

TEST_CASE("A negative texture index counts back from the last texture coordinate", "[OBJ]")
{
    // -1 is the most recent vt (0.75, 1), -2 the one before it (0.25, 0.5).
    const LoadedObj loaded = load_textured_tetrahedron("f 1/-2 3/-1 2/-1");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(1., 1e-6));
}

TEST_CASE("Texture coordinates with a w component are kept", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1/1 3/2 2/2", "vt 0.25 0.5 0\nvt 0.75 1 0\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(1., 1e-6));
}

TEST_CASE("A texture coordinate with w does not shift the indices of the ones after it", "[OBJ][Regression]")
{
    // The w on the first vt used to drop that line, so vt 2 resolved to the third coordinate.
    const LoadedObj loaded = load_textured_tetrahedron("f 1/2 3/3 2/-1", "vt 0.1 0.2 0\nvt 0.25 0.5\nvt 0.75 1\n");

    REQUIRE(loaded.ok);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(1., 1e-6));
    CHECK_THAT(uv[2].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[2].y(), WithinAbs(1., 1e-6));
}

TEST_CASE("Both triangles of a quad take the texture coordinates of their own corners", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_obj("v 0 0 0\nv 10 0 0\nv 10 10 0\nv 0 10 0\n"
                                               "vt 0.1 0.05\nvt 0.2 0.1\nvt 0.3 0.15\nvt 0.4 0.2\n"
                                               "usemtl a\n"
                                               "f 1/1 2/2 3/3 4/4\n");

    REQUIRE(loaded.ok);
    REQUIRE(loaded.mesh.facets_count() == 2);
    REQUIRE(loaded.info.uvs.size() == 2);
    check_uv_is_vt(loaded.info.uvs[0][0], 1);
    check_uv_is_vt(loaded.info.uvs[0][1], 2);
    check_uv_is_vt(loaded.info.uvs[0][2], 3);
    check_uv_is_vt(loaded.info.uvs[1][0], 1);
    check_uv_is_vt(loaded.info.uvs[1][1], 3);
    check_uv_is_vt(loaded.info.uvs[1][2], 4);
}

TEST_CASE("Texture coordinates follow the corners of a mesh that is flipped on load", "[OBJ][Regression]")
{
    // The faces wind inwards, so the loader flips them. Vertex n uses vt n.
    const LoadedObj loaded = load_textured_obj("v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n"
                                               "vt 0.1 0.05\nvt 0.2 0.1\nvt 0.3 0.15\nvt 0.4 0.2\n"
                                               "usemtl a\n"
                                               "f 1/1 2/2 3/3\nf 1/1 4/4 2/2\nf 1/1 3/3 4/4\nf 2/2 4/4 3/3\n");

    REQUIRE(loaded.ok);
    const indexed_triangle_set &its = loaded.mesh.its;
    CHECK(its_volume(its) > 0.f);
    REQUIRE(its.indices.size() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    for (size_t face = 0; face < its.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner)
            check_uv_is_vt(loaded.info.uvs[face][corner], its.indices[face][corner] + 1);
}

TEST_CASE("A face with more than four corners is triangulated", "[OBJ][Regression]")
{
    // A hexagonal prism, radius 10 mm and 5 mm tall, with one six-cornered face at each end.
    std::string body;
    for (int z : {0, 5})
        for (int i = 0; i < 6; ++i)
            body += "v " + std::to_string(10. * std::cos(i * M_PI / 3.)) + " " + std::to_string(10. * std::sin(i * M_PI / 3.)) +
                    " " + std::to_string(z) + "\n";
    body += "f 6 5 4 3 2 1\nf 7 8 9 10 11 12\n";
    for (int i = 1; i <= 6; ++i) {
        const int j = i % 6 + 1;
        body += "f " + std::to_string(i) + " " + std::to_string(j) + " " + std::to_string(j + 6) + " " + std::to_string(i + 6) + "\n";
    }
    const LoadedObj loaded = load_plain_obj(body);

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 2 * 4 + 6 * 2);
    TriangleMesh mesh = loaded.mesh;
    CHECK_THAT(mesh.volume(), WithinAbs(1.5 * std::sqrt(3.) * 100. * 5., 0.01));
    CHECK(loaded.mesh.stats().open_edges == 0);
}

TEST_CASE("A concave face with more than four corners is triangulated inside its outline", "[OBJ][Regression]")
{
    // An L, starting at the top of its upright: a fan from there would cut across the notch.
    const LoadedObj loaded = load_flat_face({{4, 10}, {0, 10}, {0, 0}, {10, 0}, {10, 4}, {4, 4}});

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    check_covers_flat_face(loaded.mesh.its, 64.);
}

TEST_CASE("A concave quad is split along the diagonal that lies inside it", "[OBJ][Regression]")
{
    // An arrowhead whose second corner is the reflex one, so the diagonal from the first to the
    // third corner runs outside it.
    const LoadedObj loaded = load_flat_face({{0, 0}, {2, 1}, {4, 0}, {2, 4}});

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 2);
    check_covers_flat_face(loaded.mesh.its, 6.);
}

TEST_CASE("A convex quad keeps its first-to-third corner diagonal", "[OBJ]")
{
    const LoadedObj loaded = load_flat_face({{0, 0}, {10, 0}, {10, 10}, {0, 10}});

    REQUIRE(loaded.ok);
    const indexed_triangle_set &its = loaded.mesh.its;
    REQUIRE(its.indices.size() == 2);
    CHECK(its.indices[0] == stl_triangle_vertex_indices(0, 1, 2));
    CHECK(its.indices[1] == stl_triangle_vertex_indices(0, 2, 3));
}

TEST_CASE("A bow-tie face is left out instead of split into overlapping triangles", "[OBJ][Regression]")
{
    // Its two halves wind opposite ways, so it encloses no area to cover.
    const LoadedObj loaded = load_triangle_and_face({{0, 0}, {2, 2}, {0, 2}, {2, 0}});

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 1);
    CHECK(loaded.info.rejected_faces == 1);
}

TEST_CASE("A face whose outline crosses itself is left out", "[OBJ][Regression]")
{
    const auto outline = GENERATE(
        // A figure eight with one loop larger than the other, so it still has a net area.
        std::vector<Vec2f>{{0, 0}, {4, 4}, {4, 0}, {0, 4}, {-1, 2}},
        // A pentagram turns left at every corner, but twice around.
        std::vector<Vec2f>{{10, 0}, {-8.09017f, 5.87785f}, {3.09017f, -9.51057f}, {3.09017f, 9.51057f}, {-8.09017f, -5.87785f}},
        // Two corners at the same place, so the outline touches itself.
        std::vector<Vec2f>{{0, 0}, {10, 0}, {5, 5}, {10, 10}, {0, 10}, {5, 5}});
    CAPTURE(outline);
    const LoadedObj loaded = load_triangle_and_face(outline);

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 1);
    CHECK(loaded.info.rejected_faces == 1);
}

TEST_CASE("A face with all corners on a line is left out", "[OBJ]")
{
    const LoadedObj loaded = load_triangle_and_face({{0, 0}, {1, 1}, {2, 2}, {3, 3}});

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 1);
    CHECK(loaded.info.rejected_faces == 1);
}

TEST_CASE("A corner repeating the one before it is dropped", "[OBJ]")
{
    const auto outline = GENERATE(std::vector<Vec2f>{{0, 0}, {10, 0}, {10, 0}, {10, 10}, {0, 10}},
                                  std::vector<Vec2f>{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
    CAPTURE(outline);
    const LoadedObj loaded = load_flat_face(outline);

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 2);
    CHECK(loaded.info.rejected_faces == 0);
    check_covers_flat_face(loaded.mesh.its, 100.);
}

TEST_CASE("A face with straight corners splits into triangles that all have area", "[OBJ]")
{
    // The first one is straight, so a fan from it would hold a triangle without area.
    const LoadedObj loaded = load_flat_face({{5, 0}, {10, 0}, {10, 10}, {5, 10}, {0, 10}, {0, 0}});

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    CHECK(loaded.info.rejected_faces == 0);
    check_covers_flat_face(loaded.mesh.its, 100.);
}

TEST_CASE("A concave face with many corners is covered by triangles wound like it", "[OBJ]")
{
    const std::vector<Vec2f> corners = star_corners(24);
    const LoadedObj          loaded  = load_flat_face(corners);

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == corners.size() - 2);
    check_covers_flat_face(loaded.mesh.its, signed_area(corners));
}

TEST_CASE("A convex face with ten thousand corners is split quickly", "[OBJ]")
{
    std::vector<Vec2f> corners;
    for (int i = 0; i < 10000; ++i)
        corners.emplace_back(float(100. * std::cos(2. * M_PI * i / 10000)), float(100. * std::sin(2. * M_PI * i / 10000)));
    const auto      start   = std::chrono::steady_clock::now();
    const LoadedObj loaded  = load_flat_face(corners);
    const double    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == corners.size() - 2);
    // The corners are written with six decimals, which moves the area by a few square micrometres.
    check_covers_flat_face(loaded.mesh.its, signed_area(corners), 1e-6 * signed_area(corners));
    // Generous for slow machines; a cubic ear search takes minutes here.
    CHECK(seconds < 10.);
}

TEST_CASE("A concave face with ten thousand corners is split in reasonable time", "[OBJ]")
{
    const std::vector<Vec2f> corners = star_corners(10000);
    const auto               start   = std::chrono::steady_clock::now();
    const LoadedObj          loaded  = load_flat_face(corners);
    const double             seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == corners.size() - 2);
    CHECK(loaded.info.rejected_faces == 0);
    check_covers_flat_face(loaded.mesh.its, signed_area(corners), 1e-6 * signed_area(corners));
    CHECK(seconds < 20.);
}

TEST_CASE("A non-convex face with more corners than can be ear clipped is left out", "[OBJ]")
{
    // The parser reads lines of up to 64 KiB, too short for 20001 distinct corner indices, so the face
    // goes 6667 times around one triangle: left turns only, but not convex.
    std::string body = "v 0 0 -10\nv 10 0 -10\nv 0 10 -10\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf";
    for (int i = 0; i < 6667; ++i)
        body += " 4 5 6";
    const auto      start   = std::chrono::steady_clock::now();
    const LoadedObj loaded  = load_plain_obj(body + "\n");
    const double    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 1);
    CHECK(loaded.info.rejected_faces == 1);
    CHECK(seconds < 10.);
}

TEST_CASE("A face after a left-out face keeps its own material", "[OBJ][Regression]")
{
    ScopedTemporaryFile obj(".obj");
    ScopedTemporaryFile mtl(".mtl");
    {
        boost::nowide::ofstream out(mtl.string());
        out << "newmtl a\nKd 1 0 0\nnewmtl b\nKd 0 0 1\n";
    }
    {
        boost::nowide::ofstream out(obj.string());
        out << "mtllib " << mtl.path().filename().string() << "\n"
            << "v 0 0 0\nv 2 2 0\nv 0 2 0\nv 2 0 0\nv 0 0 5\nv 1 0 5\nv 0 1 5\n"
            << "usemtl a\nf 1 2 3 4\nusemtl b\nf 5 6 7\n";
    }
    LoadedObj   loaded;
    std::string message;
    loaded.ok = load_obj(obj.string().c_str(), &loaded.mesh, loaded.info, message);

    REQUIRE(loaded.ok);
    CHECK(loaded.info.rejected_faces == 1);
    REQUIRE(loaded.mesh.facets_count() == 1);
    REQUIRE(loaded.info.face_colors.size() == 1);
    CHECK(loaded.info.face_colors[0][2] > loaded.info.face_colors[0][0]);
}

TEST_CASE("A long thin face with real area is split, not left out", "[OBJ][Regression]")
{
    // 1000 m by 0.1 um: an area of 0.1 mm2, far above rounding, at an aspect ratio of 1e13.
    const LoadedObj loaded = load_plain_obj("v 0 0 0\nv 1000000 0 0\nv 1000000 1e-07 0\nv 0 1e-07 0\nf 1 2 3 4\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.info.rejected_faces == 0);
    CHECK(loaded.mesh.facets_count() == 2);
    check_covers_flat_face(loaded.mesh.its, 0.1, 1e-6);
}

TEST_CASE("A triangle naming a vertex twice is left out", "[OBJ]")
{
    const LoadedObj loaded = load_plain_obj("v 0 0 0\nv 10 0 0\nv 0 10 0\nf 1 2 3\nf 1 1 2\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 1);
    CHECK(loaded.info.rejected_faces == 1);
    CHECK(loaded.info.zero_area_triangles == 0);
}

TEST_CASE("A triangle of three distinct vertices on a line is kept and counted", "[OBJ]")
{
    // It may close a T-junction between its neighbours, so leaving it out could open the mesh.
    const LoadedObj loaded = load_plain_obj("v 0 0 0\nv 10 0 0\nv 0 10 0\nv 5 0 0\nf 1 2 3\nf 1 4 2\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 2);
    CHECK(loaded.info.rejected_faces == 0);
    CHECK(loaded.info.zero_area_triangles == 1);
}

TEST_CASE("A textured face after a left-out face keeps its own material", "[OBJ][Regression]")
{
    ScopedTemporaryFile obj(".obj");
    ScopedTemporaryFile mtl(".mtl");
    ScopedTemporaryFile png(".png");
    {
        // obj_to_textured_mesh() only reads the texture's bytes; decoding happens later.
        boost::nowide::ofstream out(png.string(), std::ios::binary);
        out << "texture";
    }
    {
        boost::nowide::ofstream out(mtl.string());
        out << "newmtl a\nKd 1 0 0\nnewmtl b\nKd 0 0 1\nmap_Kd " << png.path().filename().string() << "\n";
    }
    {
        // The bow-tie under material a is left out, so the triangle under b is the mesh's first.
        boost::nowide::ofstream out(obj.string());
        out << "mtllib " << mtl.path().filename().string() << "\n"
            << "v 0 0 0\nv 2 2 0\nv 0 2 0\nv 2 0 0\nv 0 0 5\nv 1 0 5\nv 0 1 5\nvt 0.25 0.5\n"
            << "usemtl a\nf 1/1 2/1 3/1 4/1\nusemtl b\nf 5/1 6/1 7/1\n";
    }
    TriangleMesh       mesh;
    ObjInfo            info;
    ObjParser::MtlData mtl_data;
    std::string        message;
    REQUIRE(load_obj(obj.string().c_str(), &mesh, info, message, &mtl_data));
    CHECK(info.rejected_faces == 1);
    REQUIRE(mesh.facets_count() == 1);

    TexturedMesh textured;
    REQUIRE(obj_to_textured_mesh(info, mesh.its, mtl_data, obj.path().parent_path().string(), textured));
    REQUIRE(textured.material_ids.size() == 1);
    REQUIRE(textured.material_ids[0] >= 0);
    REQUIRE(size_t(textured.material_ids[0]) < mtl_data.mtl_orders.size());
    CHECK(mtl_data.mtl_orders[textured.material_ids[0]] == "b");
}
