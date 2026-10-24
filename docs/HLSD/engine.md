# Engine

`ananas-engine` is the headless slicing engine a front end drives. It links `libslic3r` only (no GUI toolkit, no
Python), reads JSON-RPC 2.0 requests, one object per line, on stdin and answers one object per line on a protocol
channel. Logs go to stderr.

```
ananas-engine --profiles <resources/profiles> [--vendor <name>] [--log-level <n>]
```

## Why

A user interface should not carry its own copy of the slicer's knowledge: the setting definitions, the machine
limits, the preset rules. The engine owns them and a front end renders what it is told. Separating the two also
draws a process boundary between the AGPL slicing core and any user interface built on top of it.

## Channel

The protocol channel is a duplicate of the original stdout. File descriptor 1 is then pointed at stderr, so a
library that prints to stdout cannot corrupt a response. On Windows the channel is stdout itself.

Requests run one at a time: `Print::process()` keeps process-global state, so two slices in one process race.
A request without an `id` is a notification and gets no response. Progress arrives as notifications while a
request runs.

Errors use the JSON-RPC codes: `-32700` invalid JSON, `-32600` not a request, `-32601` unknown method, `-32602`
invalid parameters, `-32001` a named preset does not exist, `-32000` the operation failed.

## Methods

| Method | Parameters | Result |
|---|---|---|
| `engine.info` | – | name, version, protocol number, method names |
| `presets.list` | optional `printer` | without `printer`: every instantiable printer preset with vendor, model, variant and default process; with it: the printer's compatible `processes` and `filaments` |
| `config.schema` | – | every print, filament and printer setting: key, scope, type, vector, label, tooltip, category, unit, mode, bounds, enum choices, default |
| `config.resolve` | `printer`, optional `process`, `filament`, `overrides`, `limits` | the resolved preset names, the full config as serialized strings, the effective `limits` and the limit violations |
| `limits.import` | exactly one of `printer_cfg` (text) or `moonraker` (JSON text) | the Klipper limits as Klipper resolves them, their source, and the importer's warnings |
| `limits.check` | as `config.resolve` | the effective `limits` and the limit violations |
| `model.load` | `path` | the model's objects: per object its model parts as meshes (base64 float32 vertices, uint32 indices), bounding box and mesh statistics (facets, volume, open edges, shells, repairs) |
| `model.bounds` | `path`, `transforms` | per object the bounding box and the instance offset after the transform |
| `preview.load` | `gcode` | the layers (index, z), extent, speed/flow/acceleration ranges, roles, estimated time and segment counts of a G-code file |
| `preview.chunk` | `gcode`, `layer_from`, `layer_to`, optional `travel` | the segments of a layer range as typed arrays: positions, speed, flow, acceleration, role, kind, layer |
| `analyze.stress` | `model`, optional `object` and `transform`, `material`, `voxel_size`, `supports`, `loads`, optional `tolerance`, `preconditioner`, `max_iterations`, `max_seconds` | linear stress analysis of one object: the voxel grid, solver state (`solver`: iterations, residual, converged, `preconditioner` actually used, `levels` of the multigrid hierarchy), maximum von Mises stress, minimum safety factor and maximum displacement with their positions, applied force and support reaction, `warnings`; `analyze.progress` notifications with the request id |
| `analyze.chunk` | `id`, `z_from`, `z_to` | the cell data of a range of grid layers of the latest analysis as typed arrays: solid, von Mises, safety factor, displacement, displacement vector |
| `moonraker.connect` | `host`, optional `api_key` | the printer host's state, Klipper's state and the Klipper limits as the running Klipper resolves them (`null` while it is not ready) |
| `moonraker.upload` | `host`, `gcode`, optional `name`, `print` | stores a G-code on the printer; with `print` the host starts it |
| `moonraker.start` / `moonraker.cancel` | `host`, `name` / `host` | starts a stored file / cancels the running print |
| `moonraker.status` | `host` | `state` (standby, printing, paused, complete, cancelled, error), file, progress 0..1, durations, message |
| `slice` | as `config.resolve`, plus `model` and `output` | the G-code path, estimated time, layer count, filament use, effective limits and violations; `slice.progress` notifications with the request id |

A transform is `{position:[x,y], rotation:[x,y,z], scale:[x,y,z]}`, every key optional: `position` is the centre
of the transformed object's bounding box on the bed, rotation is in degrees (applied as Rz*Ry*Rx), and the object is
always set down on the bed. `slice` takes `transforms`, one per object; without it the model keeps its layout and is
centred on the bed. `preview.*` reads the G-code itself, so it serves any file, also one the engine did not write.

`process` and `filament` default to the printer's own. `overrides` maps a setting key to its serialized value.
`limits` is an object with the Klipper limits and a `source` of `printer_cfg` or `moonraker`; it replaces the
limits derived from the profile.

## Stress analysis

`analyze.stress` answers where a part will fail under a load: a linear elastic finite-element analysis of one object,
meant for the question "does this bracket hold, and where does it break first", not for certification. Units are
mm, N and MPa; z is the build direction.

- **Frame.** The object is placed with `transform` (as in `model.bounds`, which tells the resulting box) and set down
  on the bed. Supports and loads use that frame, so the build orientation, which decides the weak direction of a
  printed part, is part of the request. `model` is the file path (the engine has no other model identifier) and
  `object` the index in the file. Only model parts count, modifiers and negative volumes do not.
- **Material.** `material` holds `youngs_modulus`, `yield_strength`, optional `poisson_ratio` (default 0.35) and
  `z_factor` (default 1, at least 0.05). The part is transversely isotropic about z: `E_z`, `G_xz` and `G_yz` are
  `z_factor` times the in-plane values. A tensile stress in z may use only `z_factor` times the yield strength
  (the layer bond); the safety factor is the smaller of that and `yield_strength / von Mises`.
- **Selection.** A support or a load names its place with `triangles` (indices into the triangles of all model parts
  in the order `model.load` lists them; a triangle listed twice counts once) and/or a `box` `{min, max}`. A box takes the
  nodes inside it with half a voxel of slack and the surface quads of the voxel model whose four corners are inside.
  A triangle takes the surface quads that look the way it looks (the quad's outward normal within 60 degrees of the
  triangle's outward normal, so the back face of a one-cell wall is not taken; a mesh wound inside out is turned around
  by the sign of its volume) and whose centre lies within 0.87 voxel of it. That reach is the halo: a selection can
  take faces up to 0.87 voxel beyond the triangle's edge, and a face that lies between two grid planes pulls in only
  the outer layer, not the side faces. A selection that finds no quad at all (neither by box nor by triangle, e.g. a
  triangle much smaller than a cell) takes the nodes within 0.87 voxel instead; if a box finds quads but a tiny triangle of the same selection finds none, that
  triangle is silently dropped (the fallback applies only when the whole selection is empty). A support
  holds the displacement components in its `axes` (default all three) at zero at the nodes of the selected quads (and
  the box nodes). A load has a `force` in N, the resultant on the selection; it is spread over the selected quads, as
  the consistent load of a uniform traction, or equally over the nodes when the selection has no quad.
- **Method.** The closed meshes are voxelized into a cubic grid of `voxel_size` (a cell is solid when its centre is
  inside; a mesh with an open or non-manifold edge, or an odd number of surface crossings along a column, is refused),
  every solid cell becomes a trilinear hexahedron, and `K u = f` is solved matrix-free with a preconditioned conjugate
  gradient (parallel over nodes with TBB) until the relative residual falls below `tolerance`
  (default 1e-6). The load is scaled to a largest component of 1 for the solve and the displacements are scaled back,
  so loads from 1e-200 N to 1e12 N neither overflow nor underflow in the sums of squares. No external solver is linked.
- **Preconditioner.** `preconditioner` is `"multigrid"` (default) or `"jacobi"`. Multigrid applies one geometric V cycle per
  iteration. The coarse levels join 2 x 2 x 2 cells of the level below; a coarse cell is a cube of twice the edge whose
  stiffness is the share of its eight children that are solid times twice the fine element matrix (rediscretised with
  filling weights, not a Galerkin product: no coarse matrix is stored, the levels stay matrix-free, and a hole, a thin wall or
  a second separate part keeps its coarse stiffness in proportion to its volume; cells outside an odd grid count as empty, a
  cell without solid children is empty and has no nodes). Prolongation is trilinear and the restriction its transpose; a coarse
  node is held where a fine node within one fine cell of it is held, so supports on odd nodes or on a few nodes survive on
  every level. Smoothing is a Chebyshev polynomial of degree 2 of the Jacobi-scaled operator over [lambda_max / 4, lambda_max]
  (lambda_max per level: largest Ritz value of a Lanczos run, 5 % added, capped by the Gershgorin bound), the same before and after the coarse correction and started from
  zero before it, so the cycle is a symmetric positive operator and the conjugate gradient stays valid. That holds only while the true lambda_max is below about 1.3 times the value used, so a finished hierarchy is tested before the solver relies on it: with fixed pseudo-random vectors x, y the cycle must give <Mx,x> > 0, <My,y> > 0 and <Mx,y> = <x,My>; the dense factorisation of the coarsest level must have pivots above 1e-12 of the largest and reproduce a test solve (supports close together can make it nearly singular). A failed test means Jacobi with a warning. Levels are added until
  at most 1 500 degrees of freedom are left; that level is solved directly (dense LDL^T, built once). Measured: 9 to 38
  iterations for all validation cases, 10 for 4.9 million cells, against 100 to 11 575 and 1 535 with Jacobi (see
  `stress-validierung.md`). The answer agrees with Jacobi to about 1e-7 of the field maximum at the same tolerance.
  If the hierarchy cannot be built (it does not fit into the memory budget, or a level does not shrink), a cycle is not
  positive definite or not finite, or the residual grows by four orders of magnitude, the solver continues with Jacobi
  (from the current iterate, with the conjugate gradient restarted: new direction p = z, rz = <r,z>) and adds a warning; `solver.preconditioner` then says `jacobi`. A stagnating multigrid run ends
  like a stagnating Jacobi run. The clock is checked in every phase: per level and Lanczos step while building, around the dense factorisation and in every cycle at every level (before the pre-smoothing and before the post-smoothing). The hierarchy costs about 22 bytes per
  solver node of the finest level for a compact body (measured, 4.9 million cells: 1.67 GB against 1.57 GB peak with Jacobi); plates and rods lose fewer nodes per level, so the estimate follows the grid dimensions (about 200 bytes per node and level plus the dense coarsest matrix) and every level is checked against the memory limit before it is allocated; when the Jacobi footprint fits but the hierarchy does not, Jacobi is used and
  the warning says so.
- **Limits.** They are the server's; a request cannot raise them. At most 5 million grid cells (solid or not) and
  10 million triangles; one grid layer (`nx * ny` cells) must fit into one result chunk of 8 MB at 25 bytes per cell
  (335 544 cells), otherwise the request is refused before any work with a hint to use a larger `voxel_size`. The
  estimated memory may not exceed 3 GB: before the voxelization the welding maps of the watertight check and the
  surface crossings of the voxelization (their number is capped by what the budget leaves, at most 50 million) are
  counted, afterwards about 330 bytes per cell of a compact body (5 million cells need roughly 1.7 GB; the multigrid hierarchy comes on top, measured +7 %, and is only built when it fits into what the limit leaves, see Preconditioner); running out of
  memory anyway answers `-32000`. At most 64 supports and 64 loads, 2 million different triangles per selection and
  20 million listed triangle indices over all supports and loads together (all checked while the request is read,
  before a list is copied; one list is also bounded by the 10 million triangles), a
  force of at most 1e12 N per component of each load and of their sum. `max_iterations` is capped at 20000,
  `max_seconds` (default 600) at 3600. `max_seconds` is the wall clock of the whole analysis from the start of the
  call: the input check, the watertight check, the voxelization, building the mesh, the selections, the check of the
  supports and the solver all look at the clock periodically and end with `-32000`; the solver also stops when the best
  residual has fallen by less than 1 % over a window of at least 3000 iterations and at least half of all iterations so
  far (with Jacobi the residual of a conjugate gradient run sits on plateaus of thousands of iterations on fine, slender grids; a
  fixed 1000-iteration window cut off runs that converged later, see `stress-validierung.md`). The evaluation after the
  solver (reaction, stress per cell, extrema) is linear in the number of cells and not part of the clock: measured
  0.1 s for 4.9 million solid cells, against about 0.07 s per Jacobi iteration there. A residual that reaches the tolerance in the iteration in which
  the clock runs out counts as a result. Every vertex must be finite. There is no way to cancel a running analysis; it
  ends by itself within these limits.
- **Supports.** Before solving, every piece of the model must be held against all three translations and all three
  rotations (checked on the held degrees of freedom of each piece, relative to the centre and the spread of its held
  nodes, so position and size of the model do not matter; three points that are not on a line, held in all axes, are
  enough); a piece without any hold, supports that leave a motion free
  (for example a face that is only held along one axis, so a sideways load slides it), or a solver that does not
  converge, stagnates or runs out of time answer `-32000` with the reason. An empty selection, a missing support or
  load, a non-finite number or more cells or memory than allowed answer `-32602`.
- **Result.** Per cell the von Mises stress at the element centre, the safety factor (capped at 1000, 0 outside the
  solid) and the displacement. `analyze.stress` returns the summary and an `id`; the cell arrays come from
  `analyze.chunk` for a range of grid layers (`z_from`..`z_to`, x fastest), base64 like `preview.chunk`, at most
  8 MB of raw data (25 bytes per cell) per call; a larger range answers `-32602` with the number of layers that
  fit. Only the latest analysis is kept.
- **Warnings.** `warnings` in the summary lists reasons to distrust the numbers: walls or ribs thinner than 4 cells
  (at least 5 % of the material, or where the stress is highest; bending locks, so the result is too stiff and too low),
  a voxel volume more than 20 % off the mesh volume (thin parts lost), negative volumes (ignored, so what they cut out
  is solid), a load that partly acts on held nodes (those parts go into the support; shown when more than 1 % of the
  input force is lost, judged per load against the sum of the load magnitudes, so loads that cancel neither hide a loss nor
  raise a false alarm), a skipped thin-wall check (it has its own time budget `warning_seconds` after the solver, which
  must be finite and above 0 and is cut to what is left of `max_seconds`, so the whole analysis stays within
  `max_seconds`; a converged result is never discarded for it), several instances (only the first is analysed), and parts that touch only at an edge or corner (a hinge in
  the voxel model).

Limits to keep in mind: a voxel model has a stepped surface, stress at a corner or under a point load grows without
bound as the grid is refined, and trilinear elements are too stiff in bending when only a few cells span the height
(a cantilever of 4 mm height is 13 % too stiff with 2 cells, 4 % with 4 cells and 1.4 % with 8 cells against beam
theory). Infill and walls are not modelled: the part is solid, so the result is an upper bound of the stiffness of a
sliced part. Eigen or CalculiX could replace the solver for larger models; they are not linked. A mechanism inside the model (parts joined only at a corner) is not found up front; the solver then stops on its own with `-32000`.

## Printer host

The engine talks to a Moonraker-compatible host (Moonraker itself, or FLUXRAKER) with libcurl: one blocking exchange per
request, a 5 s connect timeout, certificates verified, redirects not followed (a printer that redirects is not the
one that was asked for). `host` is an http or https address; without a scheme it is http, and credentials in the URL
are refused: the key goes in `api_key`, is sent as `X-Api-Key` and never logged. A host that cannot be reached, a refused
key or an HTTP error answers `-32002` with the host's own message. A stored G-code's name is a plain file name, never a
path. Nothing is sent to a host the front end did not name, and the engine never starts a print on its own.

`tests/engine/rig/rig.sh up` starts the virtual printer (real Klipper and Moonraker, file-output MCU) with its Moonraker
on `127.0.0.1` only; `test_printer_rig.py` runs the whole path against it: connect, limits, slice, upload (byte for byte),
print to the end.

## Overrides are validated, not coerced

The config deserializers read `bad` as `0` and ignore a value outside the definition's bounds. The engine
rejects an override it cannot apply as typed: a number that is not a number, a fraction for an integer, a value
outside `min`/`max`, a boolean that is not `0` or `1`, an enum value that is not one of the choices. The answer
is `-32602` with the reason, and nothing is changed. An invalid draft is never replaced by a default.

## Limits

`check_limits` compares the print speeds and accelerations with the machine limits the config carries. Machine
limits come from an imported Klipper snapshot when there is one (`printer.cfg` or Moonraker) and otherwise from the profile's machine limits; the
violation names which (`source`). A violation has a `state`: `caution` from 90 % of the limit, `exceeded` above
it. The engine reports and never clamps.

The kinds `profile` and `firmware` are reserved. The slicer caps the volumetric flow at the filament's maximum
itself, so a speed that asks for more is not a violation, and the importer already refuses Klipper values that are
out of range; no slicer setting can exceed a firmware limit.

## Slicing

A model is placed at the centre of the bed and set on it. Progress percentages from the core are not strictly
increasing between steps, so the engine reports the running maximum and ends with 100. The engine renders no
thumbnails; the G-code carries none. Apart from the thumbnail blocks and a few metadata lines of the trailing
configuration block, the output is identical to the application's command line for the same presets.

## Tests

- `tests/engine/test_limits.cpp` covers the limit rules (Catch2, suite `engine`).
- `tests/engine/test_engine_protocol.py` drives the binary over the protocol: errors, presets, schema, limits,
  overrides and a real slice (CTest `engine_protocol`, label `RequiresApp`).
- `tests/virtual-printer/run.sh` with `ANANAS_ENGINE_BIN` set slices through the engine and checks the result with
  real Klipper and Moonraker.
