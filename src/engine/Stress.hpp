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

// Server-side limits; the request cannot raise them. The memory need is about 330 bytes per cell of a compact body
// (3 dof per node in 7 vectors of 8 bytes, the element and node index arrays, the result arrays), so 5 million cells
// take roughly 1.7 GB; the multigrid hierarchy adds about 7 % when it fits (otherwise Jacobi is used).
constexpr size_t STRESS_MAX_CELLS      = 5000000;
constexpr size_t STRESS_MAX_BYTES      = size_t(3) << 30;
constexpr size_t STRESS_MAX_ITERATIONS = 20000;
constexpr double STRESS_MAX_SECONDS    = 3600.;
constexpr size_t STRESS_MAX_TRIANGLES  = 10000000;
constexpr size_t STRESS_MAX_REGIONS    = 64;      // supports, and loads, each
constexpr size_t STRESS_MAX_SELECTION_TRIANGLES = 2000000; // different triangles in one selection (duplicates count once)
constexpr size_t STRESS_MAX_SELECTION_INDICES = 20000000; // triangle indices listed over all supports and loads together (8 bytes each; 160 MB)
constexpr double STRESS_MAX_FORCE      = 1e12;    // N, per component of a load and of the sum of all loads
// The result is fetched in chunks of whole grid layers, 25 bytes per cell (solid 1, von Mises 4, safety 4,
// displacement 4, displacement vector 12) and at most STRESS_CHUNK_MAX_BYTES raw bytes per chunk. One layer of the
// grid must fit into one chunk, which limits nx * ny to 8 MB / 25 = 335 544 cells.
constexpr size_t STRESS_CHUNK_MAX_BYTES     = size_t(8) << 20;
constexpr size_t STRESS_CHUNK_BYTES_PER_CELL = 25;

// The preconditioner of the conjugate gradient. Multigrid is the default; Jacobi is the fallback (and the reference
// the multigrid result is compared with).
enum class StressPreconditioner { Multigrid, Jacobi };

struct StressInput
{
    std::vector<indexed_triangle_set> parts; // closed meshes in the analysis frame (the bed frame)
    StressMaterial                    material;
    double                            voxel_size{ 1. }; // mm
    std::vector<StressSupport>        supports;
    std::vector<StressLoad>           loads;
    double                            tolerance{ 1e-6 };  // relative residual of the solver
    StressPreconditioner              preconditioner{ StressPreconditioner::Multigrid };
    int                               fault_for_test{ 0 }; // 1: the multigrid setup fails, 2: the first cycles give a non-finite result; not part of the protocol
    size_t                            max_iterations{ STRESS_MAX_ITERATIONS }; // capped at STRESS_MAX_ITERATIONS
    size_t                            max_cells{ STRESS_MAX_CELLS };               // cells of the voxel grid (solid or not)
    size_t                            max_bytes{ STRESS_MAX_BYTES };               // estimated memory of the analysis
    double                            max_seconds{ 600. };                         // wall clock of the whole analysis from the call on (checking, voxelizing, building, solving); capped at STRESS_MAX_SECONDS
    double                            warning_seconds{ 60. };                      // own time budget of the thin-wall check after the solver (finite, above 0; also bounded by what is left of max_seconds); when it runs out the result is kept and a note is added
    size_t                            max_selection_triangles{ STRESS_MAX_SELECTION_TRIANGLES }; // capped at the same constant
    std::vector<std::string>          warnings;                                    // passed on into the result, before the solver's own
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
    StressPreconditioner preconditioner{ StressPreconditioner::Jacobi }; // what the solver ended up using (multigrid may fall back to Jacobi)
    size_t multigrid_levels{ 0 };                                         // levels of the hierarchy including the finest, 0 for Jacobi

    double max_von_mises{ 0. };
    Vec3d  max_von_mises_at{ Vec3d::Zero() };
    double min_safety{ 0. };
    Vec3d  min_safety_at{ Vec3d::Zero() };
    double max_displacement{ 0. };
    Vec3d  max_displacement_at{ Vec3d::Zero() };
    Vec3d  applied_force{ Vec3d::Zero() }; // what reached free nodes (a load on a fixed node is dropped)
    Vec3d  reaction{ Vec3d::Zero() };      // sum of the support reactions; equals -applied_force when in equilibrium

    std::vector<std::string> warnings; // things that make the numbers less trustworthy, for the user to read
};

constexpr double STRESS_SAFETY_CAP = 1000.;

// Called with a percentage (0..100, not strictly increasing) and a short message.
using StressProgress = std::function<void(int, const std::string&)>;

// Linear elastic analysis: voxelizes the closed meshes into a cubic grid (a cell is solid when its centre is
// inside), meshes the solid with trilinear hexahedra, and solves K u = f with a matrix-free conjugate
// gradient (geometric multigrid V cycle as preconditioner, Jacobi as fallback). Stress is the value at the element centre. Throws EngineError: invalid
// params for a bad input, engine error for a mesh that is not watertight, a model without volume or a solver
// that does not converge (the supports must hold all three translations and rotations, which is checked up front, per
// piece, so a failure to converge afterwards means a fine or slender grid, a joint the check cannot see, or too little
// time or too few iterations, and the message says so). Limits on cells, grid layer size, memory (also
// of the checks before the voxelization), number of supports, loads and selected triangles, force size, iterations,
// stagnation and wall-clock time (from the call on, in every long phase) end in an error instead of a runaway;
// running out of memory is an engine error too. A load is selected by its surface faces: a triangle takes the
// faces of the voxel surface that face it and lie within reach, a box the faces with all four corners inside.
StressResult solve_stress(const StressInput& input, const StressProgress& progress = nullptr);

} // namespace Slic3r::engine
