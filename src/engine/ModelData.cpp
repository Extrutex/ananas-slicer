#include "ModelData.hpp"

#include "Base64.hpp"
#include "Engine.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>

#include <cmath>

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r::engine {

namespace {

constexpr double MAX_SCALE = 1000.0;

[[noreturn]] void invalid(const std::string& message)
{
    throw EngineError(ErrorCode::InvalidParams, message);
}

Vec3d read_vec3(const json& value, const char* name, double fallback)
{
    if (!value.is_array() || value.size() != 3)
        invalid(std::string("transform.") + name + " must be an array of three numbers");
    Vec3d out(fallback, fallback, fallback);
    for (size_t i = 0; i < 3; ++i) {
        if (!value[i].is_number() || !std::isfinite(value[i].get<double>()))
            invalid(std::string("transform.") + name + " must contain finite numbers");
        out[i] = value[i].get<double>();
    }
    return out;
}

template<class T> std::string encode(const std::vector<T>& values)
{
    return base64_encode(values.data(), values.size() * sizeof(T));
}

} // namespace

ObjectTransform parse_transform(const json& transform)
{
    ObjectTransform out;
    if (transform.is_null())
        return out;
    if (!transform.is_object())
        invalid("a transform must be an object");
    if (transform.contains("position")) {
        const json& p = transform.at("position");
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() || !std::isfinite(p[0].get<double>()) ||
            !std::isfinite(p[1].get<double>()))
            invalid("transform.position must be [x, y]");
        out.position = Vec2d(p[0].get<double>(), p[1].get<double>());
    }
    if (transform.contains("rotation"))
        out.rotation_deg = read_vec3(transform.at("rotation"), "rotation", 0.0);
    if (transform.contains("scale")) {
        out.scale = read_vec3(transform.at("scale"), "scale", 1.0);
        for (size_t i = 0; i < 3; ++i)
            if (out.scale[i] <= 0.0 || out.scale[i] > MAX_SCALE)
                invalid("transform.scale must be above 0 and at most 1000");
    }
    return out;
}

BoundingBoxf3 apply_transform(ModelObject& object, const ObjectTransform& transform, const Vec2d& default_center)
{
    ModelInstance& instance = *object.instances.front();
    instance.set_offset(Vec3d::Zero());
    instance.set_rotation(Vec3d(transform.rotation_deg.x(), transform.rotation_deg.y(), transform.rotation_deg.z()) * (M_PI / 180.0));
    instance.set_scaling_factor(transform.scale);

    // Rotation and scale move the bounding box centre; place the centre where the caller asked.
    const Vec3d   center = object.instance_bounding_box(0).center();
    const Vec2d   target = transform.position.value_or(default_center);
    instance.set_offset(Vec3d(target.x() - center.x(), target.y() - center.y(), 0.0));
    object.invalidate_bounding_box();
    object.ensure_on_bed();
    return object.instance_bounding_box(0);
}

json bounding_box_json(const BoundingBoxf3& box)
{
    return { { "min", { box.min.x(), box.min.y(), box.min.z() } }, { "max", { box.max.x(), box.max.y(), box.max.z() } } };
}

namespace {

json mesh_arrays_json(const TriangleMesh& mesh)
{
    std::vector<float> vertices;
    vertices.reserve(mesh.its.vertices.size() * 3);
    for (const stl_vertex& v : mesh.its.vertices)
        vertices.insert(vertices.end(), { v.x(), v.y(), v.z() });
    std::vector<uint32_t> indices;
    indices.reserve(mesh.its.indices.size() * 3);
    for (const stl_triangle_vertex_indices& t : mesh.its.indices)
        indices.insert(indices.end(), { uint32_t(t[0]), uint32_t(t[1]), uint32_t(t[2]) });
    return { { "vertices", encode(vertices) },
             { "indices", encode(indices) },
             { "triangles", mesh.its.indices.size() },
             { "bbox", bounding_box_json(mesh.bounding_box()) } };
}

} // namespace

json object_mesh_json(const ModelObject& object)
{
    // One mesh per model part, already in the object's coordinates: a front end can draw them apart
    // (exploded view) or together, and the object's transform moves them all.
    json parts = json::array();
    for (const ModelVolume* volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        TriangleMesh mesh = volume->mesh();
        mesh.transform(volume->get_matrix());
        json part = mesh_arrays_json(mesh);
        part["name"] = volume->name;
        parts.push_back(std::move(part));
    }

    // The mesh as the slicer sees it, for the inspection figures.
    const TriangleMesh       whole = object.raw_mesh();
    const TriangleMeshStats& stats = whole.stats();
    // Where the file puts the object: the centre of its bounding box and its rotation and scale. A file
    // with several objects keeps its layout this way.
    json placement = nullptr;
    if (!object.instances.empty()) {
        const ModelInstance& instance = *object.instances.front();
        const BoundingBoxf3  box      = object.instance_bounding_box(0);
        const Vec3d          rotation = instance.get_rotation() * (180.0 / M_PI);
        const Vec3d          scale    = instance.get_scaling_factor();
        placement = { { "position", { box.center().x(), box.center().y() } },
                      { "rotation", { rotation.x(), rotation.y(), rotation.z() } },
                      { "scale", { scale.x(), scale.y(), scale.z() } },
                      { "bbox", bounding_box_json(box) } };
    }
    return { { "name", object.name },
             { "placement", std::move(placement) },
             { "parts", std::move(parts) },
             { "bbox", bounding_box_json(whole.bounding_box()) },
             { "stats",
               { { "facets", stats.number_of_facets },
                 { "volume", stats.volume },
                 { "open_edges", stats.open_edges },
                 { "shells", stats.number_of_parts },
                 { "repaired",
                   { { "degenerate_facets", stats.repaired_errors.degenerate_facets },
                     { "edges_fixed", stats.repaired_errors.edges_fixed },
                     { "facets_removed", stats.repaired_errors.facets_removed },
                     { "facets_reversed", stats.repaired_errors.facets_reversed },
                     { "backwards_edges", stats.repaired_errors.backwards_edges } } } } } };
}

Model ModelCache::copy_of(const std::string& path)
{
    const fs::path file(path);
    boost::system::error_code ec;
    const std::time_t         modified = fs::last_write_time(file, ec);
    const std::uintmax_t      size     = fs::file_size(file, ec);

    auto found = m_entries.find(path);
    if (found == m_entries.end() || found->second.modified != modified || found->second.size != size) {
        Entry entry;
        entry.modified = modified;
        entry.size     = size;
        // The 3MF reader only returns the model when asked for it; the other formats ignore the flags.
        entry.model = std::make_shared<Model>(Model::read_from_file(
            path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
        found          = m_entries.insert_or_assign(path, std::move(entry)).first;
    }
    return *found->second.model;
}

} // namespace Slic3r::engine
