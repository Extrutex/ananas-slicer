#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace Slic3r::engine {

// Where an object stands on the bed. `position` is the XY of the centre of the transformed object's bounding
// box; the object is always set down on the bed (lowest point at z = 0). Rotation is in degrees, applied as
// Rz * Ry * Rx, then scaled; both are about the object's own origin.
struct ObjectTransform
{
    std::optional<Vec2d> position;
    Vec3d                rotation_deg{ Vec3d::Zero() };
    Vec3d                scale{ Vec3d::Ones() };
};

// Reads {"position":[x,y], "rotation":[x,y,z], "scale":[x,y,z]}. Every key is optional. Throws EngineError
// (invalid params) on a wrong shape, a non-finite number or a scale outside (0, 1000].
ObjectTransform parse_transform(const nlohmann::json& transform);

// Applies the transform to the object's first instance. Without a position the object is centred on
// `default_center`. Returns the object's bounding box afterwards; the instance offset that results is
// `object.instances.front()->get_offset()`.
BoundingBoxf3 apply_transform(ModelObject& object, const ObjectTransform& transform, const Vec2d& default_center);

nlohmann::json bounding_box_json(const BoundingBoxf3& box);

// The object without its instance transform: {"name", "placement", "bbox", "stats", "parts"}; `placement` is
// where the file puts it ({"position", "rotation", "scale", "bbox"}). Every model part is
// {"name", "vertices" (base64 float32 x3), "indices" (base64 uint32 x3), "triangles", "bbox"}. `stats` is what
// the inspection view shows: facets, volume (mm^3), open edges, shells and the repairs made on load.
nlohmann::json object_mesh_json(const ModelObject& object);

// Models read from a file, kept while the file stays unchanged, so moving an object does not read it again.
class ModelCache
{
public:
    // A private copy that the caller may transform. Throws on a file that cannot be read.
    Model copy_of(const std::string& path);

private:
    struct Entry
    {
        std::time_t           modified{ 0 };
        std::uintmax_t        size{ 0 };
        std::shared_ptr<Model> model;
    };
    std::map<std::string, Entry> m_entries;
};

} // namespace Slic3r::engine
