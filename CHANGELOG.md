# Changelog

All notable changes to ANANAS SLICER are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). ANANAS SLICER forked OrcaSlicer
2.5.0-dev on 30 September 2026 (as KLIPSLICE); everything below is relative to
that base. Upstream fixes carried over are marked *backport* with the OrcaSlicer
pull request number in the commit history.

## [Unreleased]

## [0.1.0-alpha.1] - unreleased

First public release of the classic desktop app. Alpha: settings, profiles and
file formats can still change without migration. The release packages are
unsigned; see the [download page](https://extrutex.github.io/ananas-slicer-wiki/download/)
for the macOS and Windows steps.

### Klipper only
- The G-code flavor is locked to `klipper`; projects and presets with another
  flavor convert when they load.
- The G-code writer speaks Klipper only: motion limits, temperatures, fans,
  tools and retraction. Non-Klipper branches of G-code generation and of print
  validation are gone.
- `MINIMUM_CRUISE_RATIO` replaces the removed `ACCEL_TO_DECEL`; per-filament
  firmware retraction is sent with `SET_RETRACTION`; firmware retraction follows
  custom G-code; the nozzle wipes dry after `G10`.
- Klipper motion state and pressure advance are restored after the print, only
  on the extruder steppers that exist, and only for names Klipper can address.
- Layer progress is reported to `print_stats`; the first-layer time in the
  header is correct.
- Klipper owns `square_corner_velocity` and `minimum_cruise_ratio`; Voron
  profiles estimate corners with Klipper's stock value.
- Print time is estimated with a model of Klipper v0.13 motion planning and
  checked against Klipper's batch mode in the test suite.
- Klipper limits import from `printer.cfg` and from Moonraker: comments,
  continuation lines, includes and the `SAVE_CONFIG` block are read the way
  Klipper reads them.

### Moonraker only
- Every other print host is removed: the OctoPrint family, PrusaLink, Duet,
  Creality, Elegoo, Flashforge, the LAN backends and the cloud hosts. The host
  type is Moonraker.
- Moonraker is discovered on the LAN through Bonjour; `.local` names resolve on
  Windows too.
- Upload reads the path Moonraker returns; upload-and-print leaves the start to
  Moonraker; `CANCEL_PRINT` restores the print state and keeps the heaters off.
- A Moonraker status strip sits above the Device tab; filament sync reads the
  Klipper filament changer directly.

### Removed from the fork
- Bambu network plug-in, login, cloud provider, user manager, model mall,
  telemetry, bind/HMS/firmware-upgrade dialogs, camera, live view, SD browser
  and FFmpeg.
- Bambu vendor gates in config and presets, `scan_first_layer`,
  `bbl_calib_mark_logo`, the Bambu device model and the printer-agent layer.
- The update check and profile OTA are switched off; the app opens no network
  connection at start.

### Printer profiles
- Only printers that verifiably run Klipper ship with a profile: 174 models
  from 41 vendors, each with per-model evidence in
  `docs/ananas-slicer/printer-firmware.json` (201 models reviewed, 27 dropped).
- Every machine profile carries the Klipper flavor and the Moonraker host.
  Own and converted machines use **Generic Klipper Printer** or **Create printer**.
- RatRig single-toolhead start G-code passes `EXTRUDER_OTHER_LAYER_TEMP`; Qidi
  start G-code heats the bed for the selected plate type (*backports*).

### Slicing
- Precise outer wall as a toolpath shift (`precise_outer_wall_method`,
  experimental), also behind a single outer wall; Arachne wall geometry is held
  to `min_bead_width` by a test oracle.
- Tree support: deterministic overhang detection, the configured XY distance is
  kept, no tail support under bed-standing feet.
- The overhang speed curve is monotonic; hops inside a top surface and ironing
  keep the top-surface acceleration and jerk.
- Reproducible output: exporting the same slice twice gives the same bytes,
  painted multi-material segmentation is deterministic, no scaled values are
  cached in statics.
- Slicing fails cleanly when the layers cannot cover the object height; layers
  no longer leak on re-slice.
- OBJ import handles faces with any number of corners, keeps texture materials
  aligned and thin faces, and drops degenerate faces.
- Thumbnails are written on command-line slices; JPEG thumbnails are no longer
  truncated.
- 45 fixes carried over from OrcaSlicer: crashes on malformed projects
  and command lines, 3MF component cycles, STL/AMF/PNG edge cases, an mcut hang,
  a gap-fill leak, config file locking and atomic writes, shrinkage compensation
  on the first slice, the build height check, ironing spacing, internal bridges,
  thin-walled holes, mirrored transforms and more.

### App
- Renamed from KLIPSLICE to ANANAS SLICER: own app key `AnanasSlicer`, data
  directory, binaries, bundle, logo, splash, About dialog and a dark industrial
  theme.
- The first start copies the KLIPSLICE settings directory once, with links and
  permissions; the Flatpak finds the old sandbox; a failed copy never starts a
  fresh data directory.
- The gettext domain is independent of the app key; installer, ProgID, URL
  schemes and D-Bus names do not collide with OrcaSlicer.
- The TBB pool start-up barrier is bounded (no hang at start).

### Tests and CI
- A virtual Klipper + Moonraker rig checks the G-code end to end: upload, print,
  cancel, thumbnails and the state macros.
- Every push builds and tests on Linux, Windows and macOS; the macOS slicer is
  configured with the SDK its dependencies were built with.

### Developer preview (built from source, not in the release packages)
- `ananas-engine`: a headless slicing engine with a JSON-RPC protocol (presets,
  schema, limits with their source, slice, model and preview data, a Moonraker
  client and a virtual printer).
- Cockpit UI (Tauri and React): parameter table with every limit and its
  source, 3D viewport with three views and an exploded view, G-code heatmaps for
  speed, flow and acceleration, printer connection, upload and print with native
  file dialogs.
- Force distribution: linear stress analysis on voxel hexahedra with a
  matrix-free conjugate gradient and a geometric multigrid preconditioner,
  validated against a tension bar, a cantilever, three-point bending, a thick
  tube, a plate with a hole and an L bracket. Results are guide values from a
  solid-body model.

[Unreleased]: https://github.com/Extrutex/ananas-slicer/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/Extrutex/ananas-slicer/releases/tag/v0.1.0-alpha.1
