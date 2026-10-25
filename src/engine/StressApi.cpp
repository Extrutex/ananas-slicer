#include "StressApi.hpp"

#include "Base64.hpp"
#include "Engine.hpp"
#include "ModelData.hpp"

#include <cmath>

using json = nlohmann::json;

namespace Slic3r::engine {

namespace {

[[noreturn]] void invalid(const std::string& message)
{
    throw EngineError(ErrorCode::InvalidParams, message);
}

double number(const json& value, const std::string& name)
{
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        invalid(name + " must be a finite number");
    return value.get<double>();
}

Vec3d vec3(const json& value, const std::string& name)
{
    if (!value.is_array() || value.size() != 3)
        invalid(name + " must be an array of three numbers");
    return Vec3d(number(value[0], name), number(value[1], name), number(value[2], name));
}

template<class T> std::string encode(const std::vector<T>& values)
{
    return base64_encode(values.data(), values.size() * sizeof(T));
}

// `indices_left` is what the whole request may still list as triangle indices; it is checked against the length of a
// list BEFORE the list is copied, so a hostile request does not first allocate memory and fail afterwards.
StressSelection parse_selection(const json& entry, const std::string& name, size_t& indices_left)
{
    StressSelection out;
    if (entry.contains("triangles")) {
        const json& list = entry.at("triangles");
        if (!list.is_array())
            invalid(name + ".triangles must be an array of triangle indices");
        if (list.size() > STRESS_MAX_TRIANGLES)
            invalid(name + ".triangles lists more than " + std::to_string(STRESS_MAX_TRIANGLES) + " triangles");
        if (list.size() > indices_left)
            invalid(name + ".triangles: all supports and loads together may list at most " + std::to_string(STRESS_MAX_SELECTION_INDICES) +
                    " triangle indices");
        indices_left -= list.size();
        out.triangles.reserve(list.size());
        for (const json& id : list) {
            if (!id.is_number_unsigned())
                invalid(name + ".triangles must hold non-negative integers");
            out.triangles.push_back(id.get<size_t>());
        }
    }
    if (entry.contains("box")) {
        const json& box = entry.at("box");
        if (!box.is_object() || !box.contains("min") || !box.contains("max"))
            invalid(name + ".box must be {min:[x,y,z], max:[x,y,z]}");
        const Vec3d lo = vec3(box.at("min"), name + ".box.min"), hi = vec3(box.at("max"), name + ".box.max");
        if ((lo.array() > hi.array()).any())
            invalid(name + ".box.min must not exceed .max");
        out.box = BoundingBoxf3(lo, hi);
    }
    return out;
}

} // namespace

StressInput parse_stress_request(const json& params, Model& model)
{
    if (!params.is_object())
        invalid("params must be an object");
    StressInput input;

    size_t index = 0;
    if (params.contains("object")) {
        if (!params.at("object").is_number_unsigned())
            invalid("\"object\" must be a non-negative integer");
        index = params.at("object").get<size_t>();
    }
    if (model.objects.empty())
        throw EngineError(ErrorCode::EngineError, "the model file contains no objects");
    if (index >= model.objects.size())
        invalid("\"object\" is outside 0.." + std::to_string(model.objects.size() - 1));
    ModelObject& object = *model.objects[index];
    if (object.instances.empty())
        throw EngineError(ErrorCode::EngineError, "the object has no instance");
    if (object.instances.size() > 1)
        input.warnings.push_back("The object has " + std::to_string(object.instances.size()) + " instances; only the first is analysed.");
    apply_transform(object, params.contains("transform") ? parse_transform(params.at("transform")) : ObjectTransform{}, Vec2d::Zero());
    const Transform3d placement = object.instances.front()->get_matrix();
    size_t negative = 0;
    for (const ModelVolume* volume : object.volumes) {
        if (volume->is_negative_volume())
            ++negative;
        if (!volume->is_model_part())
            continue;
        TriangleMesh mesh = volume->mesh();
        mesh.transform(volume->get_matrix());
        mesh.transform(placement);
        input.parts.push_back(mesh.its);
    }

    if (negative > 0)
        input.warnings.push_back(std::to_string(negative) + " negative volume(s) are ignored: what they cut out of the model is solid in the analysis.");

    if (!params.contains("material") || !params.at("material").is_object())
        invalid("missing parameter \"material\" (youngs_modulus, yield_strength, optional poisson_ratio, z_factor)");
    const json& material = params.at("material");
    for (const char* key : { "youngs_modulus", "yield_strength" })
        if (!material.contains(key))
            invalid(std::string("material.") + key + " is missing");
    input.material.youngs_modulus = number(material.at("youngs_modulus"), "material.youngs_modulus");
    input.material.yield_strength = number(material.at("yield_strength"), "material.yield_strength");
    if (material.contains("poisson_ratio"))
        input.material.poisson_ratio = number(material.at("poisson_ratio"), "material.poisson_ratio");
    if (material.contains("z_factor"))
        input.material.z_factor = number(material.at("z_factor"), "material.z_factor");

    if (!params.contains("voxel_size"))
        invalid("missing parameter \"voxel_size\"");
    input.voxel_size = number(params.at("voxel_size"), "voxel_size");
    if (params.contains("tolerance"))
        input.tolerance = number(params.at("tolerance"), "tolerance");
    if (params.contains("preconditioner")) {
        const json& value = params.at("preconditioner");
        if (!value.is_string() || (value.get<std::string>() != "multigrid" && value.get<std::string>() != "jacobi"))
            invalid("\"preconditioner\" must be \"multigrid\" or \"jacobi\"");
        input.preconditioner = value.get<std::string>() == "jacobi" ? StressPreconditioner::Jacobi : StressPreconditioner::Multigrid;
    }
    if (params.contains("max_iterations")) {
        if (!params.at("max_iterations").is_number_unsigned())
            invalid("\"max_iterations\" must be a non-negative integer");
        input.max_iterations = params.at("max_iterations").get<size_t>();
    }

    if (params.contains("max_seconds"))
        input.max_seconds = number(params.at("max_seconds"), "max_seconds");

    const auto list = [&](const char* key) -> const json& {
        static const json empty = json::array();
        if (!params.contains(key))
            return empty;
        if (!params.at(key).is_array())
            invalid(std::string("\"") + key + "\" must be an array");
        return params.at(key);
    };
    // The number of regions is checked before any of them is parsed and copied.
    const json& supports = list("supports");
    const json& loads    = list("loads");
    if (supports.size() > STRESS_MAX_REGIONS || loads.size() > STRESS_MAX_REGIONS)
        invalid("at most " + std::to_string(STRESS_MAX_REGIONS) + " supports and " + std::to_string(STRESS_MAX_REGIONS) + " loads are allowed");
    size_t indices_left = STRESS_MAX_SELECTION_INDICES;
    size_t n = 0;
    for (const json& entry : supports) {
        const std::string name = "supports[" + std::to_string(n++) + "]";
        if (!entry.is_object())
            invalid(name + " must be an object");
        StressSupport support;
        support.where = parse_selection(entry, name, indices_left);
        if (entry.contains("axes")) {
            const json& axes = entry.at("axes");
            if (!axes.is_array() || axes.size() != 3 || !axes[0].is_boolean() || !axes[1].is_boolean() || !axes[2].is_boolean())
                invalid(name + ".axes must be [x, y, z] of booleans");
            for (int a = 0; a < 3; ++a)
                support.axes[a] = axes[a].get<bool>();
        }
        input.supports.push_back(std::move(support));
    }
    n = 0;
    for (const json& entry : loads) {
        const std::string name = "loads[" + std::to_string(n++) + "]";
        if (!entry.is_object() || !entry.contains("force"))
            invalid(name + " must be an object with \"force\" [fx, fy, fz] in N");
        StressLoad load;
        load.where = parse_selection(entry, name, indices_left);
        load.force = vec3(entry.at("force"), name + ".force");
        input.loads.push_back(std::move(load));
    }
    return input;
}

json stress_summary_json(size_t id, const StressResult& r)
{
    const auto v3 = [](const Vec3d& v) { return json{ v.x(), v.y(), v.z() }; };
    return { { "id", id },
             { "grid",
               { { "size", { r.size[0], r.size[1], r.size[2] } }, { "voxel_size", r.voxel_size }, { "origin", v3(r.origin) } } },
             { "solid_cells", r.solid_cells },
             { "nodes", r.nodes },
             { "fixed_dofs", r.fixed_dofs },
             { "solver", { { "iterations", r.iterations },
                           { "residual", r.residual },
                           { "converged", r.converged },
                           { "preconditioner", r.preconditioner == StressPreconditioner::Multigrid ? "multigrid" : "jacobi" },
                           { "levels", r.multigrid_levels } } },
             { "max_von_mises", { { "value", r.max_von_mises }, { "at", v3(r.max_von_mises_at) } } },
             { "min_safety", { { "value", r.min_safety }, { "at", v3(r.min_safety_at) } } },
             { "max_displacement", { { "value", r.max_displacement }, { "at", v3(r.max_displacement_at) } } },
             { "applied_force", v3(r.applied_force) },
             { "reaction", v3(r.reaction) },
             { "safety_cap", STRESS_SAFETY_CAP },
             { "warnings", r.warnings } };
}

json stress_chunk_json(size_t id, const StressResult& r, size_t z_from, size_t z_to, size_t max_bytes)
{
    if (z_from > z_to || z_to >= r.size[2])
        throw EngineError(ErrorCode::InvalidParams, "layer range is outside 0.." + std::to_string(r.size[2] - 1));
    const size_t layer = r.size[0] * r.size[1], first = z_from * layer, count = (z_to - z_from + 1) * layer;
    constexpr size_t BYTES_PER_CELL = STRESS_CHUNK_BYTES_PER_CELL;
    if (count > max_bytes / BYTES_PER_CELL) {
        const size_t layers = max_bytes / BYTES_PER_CELL / std::max<size_t>(layer, 1);
        throw EngineError(ErrorCode::InvalidParams,
                          layers == 0 ? "a single grid layer has " + std::to_string(layer) + " cells, more than one chunk of " +
                                            std::to_string(max_bytes >> 20) + " MB holds; use a larger voxel_size"
                                      : "the layer range holds " + std::to_string(z_to - z_from + 1) + " layers, at most " +
                                            std::to_string(layers) + " fit into one chunk of " + std::to_string(max_bytes >> 20) +
                                            " MB; ask for a smaller z range");
    }
    const auto   slice = [&](const auto& values, size_t per_cell) {
        using T = typename std::decay_t<decltype(values)>::value_type;
        return std::vector<T>(values.begin() + first * per_cell, values.begin() + (first + count) * per_cell);
    };
    return { { "id", id },
             { "z_from", z_from },
             { "z_to", z_to },
             { "count", count },
             { "solid", encode(slice(r.solid, 1)) },
             { "von_mises", encode(slice(r.von_mises, 1)) },
             { "safety", encode(slice(r.safety, 1)) },
             { "displacement", encode(slice(r.displacement, 1)) },
             { "displacement_vec", encode(slice(r.displacement_vec, 3)) } };
}

} // namespace Slic3r::engine
