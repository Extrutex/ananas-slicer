#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::engine {

// Units: mm, N, MPa. The z axis is the build direction.
struct StressMaterial
{
    double youngs_modulus{ 2000. }; // MPa, in the layer plane (x, y)
    double poisson_ratio{ 0.35 };   // 0 <= nu < 0.5
    double yield_strength{ 40. };   // MPa, in the layer plane
    // Weakening across the layers: the stiffness in z (E_z, G_xz, G_yz) is this factor times the in-plane value,
    // and a tensile stress in z is allowed only this fraction of the yield strength. 1 = isotropic.
    double z_factor{ 1. };
};

// Where a support or a load acts: the nodes of the voxel model near some triangles (indices into all model-part
// triangles, parts in order) and/or inside a box (same coordinates as the triangles).
struct StressSelection
{
    std::vector<size_t>          triangles;
    std::optional<BoundingBoxf3> box;
};

struct StressSupport
{
    StressSelection       where;
    std::array<bool, 3>   axes{ true, true, true }; // the displacement components held at zero
};

struct StressLoad
{
    StressSelection where;
    Vec3d           force{ Vec3d::Zero() }; // N, the resultant on the selection
};

struct StressInput
{
    std::vector<indexed_triangle_set> parts; // closed meshes in the analysis frame (the bed frame)
    StressMaterial                    material;
    double                            voxel_size{ 1. }; // mm
    std::vector<StressSupport>        supports;
    std::vector<StressLoad>           loads;
    double                            tolerance{ 1e-6 };  // relative residual of the solver
    size_t                            max_iterations{ 20000 };
    size_t                            max_cells{ 20000000 }; // cells of the voxel grid (solid or not)
};

// Voxel arrays are cell-indexed, x running fastest: index = i + nx * (j + ny * k).
struct StressResult
{
    std::array<size_t, 3> size{ 0, 0, 0 };
    double                voxel_size{ 0. };
    Vec3d                 origin{ Vec3d::Zero() };

    std::vector<uint8_t> solid;
    std::vector<float>   von_mises;         // MPa
    std::vector<float>   safety;            // yield strength / stress, capped at safety_cap; 0 outside the solid
    std::vector<float>   displacement;      // mm, length of the cell's mean displacement
    std::vector<float>   displacement_vec;  // mm, 3 per cell

    size_t solid_cells{ 0 };
    size_t nodes{ 0 };
    size_t fixed_dofs{ 0 };
    size_t iterations{ 0 };
    double residual{ 0. }; // final relative residual
    bool   converged{ false };

    double max_von_mises{ 0. };
    Vec3d  max_von_mises_at{ Vec3d::Zero() };
    double min_safety{ 0. };
    Vec3d  min_safety_at{ Vec3d::Zero() };
    double max_displacement{ 0. };
    Vec3d  max_displacement_at{ Vec3d::Zero() };
    Vec3d  applied_force{ Vec3d::Zero() }; // what reached free nodes (a load on a fixed node is dropped)
    Vec3d  reaction{ Vec3d::Zero() };      // sum of the support reactions; equals -applied_force when in equilibrium
};

constexpr double STRESS_SAFETY_CAP = 1000.;

// Called with a percentage (0..100, not strictly increasing) and a short message.
using StressProgress = std::function<void(int, const std::string&)>;

// Linear elastic analysis: voxelizes the closed meshes into a cubic grid (a cell is solid when its centre is
// inside), meshes the solid with trilinear hexahedra, and solves K u = f with a matrix-free conjugate
// gradient (Jacobi preconditioner). Stress is the value at the element centre. Throws EngineError: invalid
// params for a bad input, engine error for a mesh that is not watertight, a model without volume or a solver
// that does not converge (typically supports that leave the body free to move).
StressResult solve_stress(const StressInput& input, const StressProgress& progress = nullptr);

} // namespace Slic3r::engine
