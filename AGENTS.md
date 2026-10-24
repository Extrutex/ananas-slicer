# CLAUDE.md

ANANAS SLICER — an open-source (AGPL-3.0) slicer for printers that run Klipper, and
only for those. A hard fork of OrcaSlicer: C++17, wxWidgets GUI (the classic app),
CMake. New in this fork: a headless engine (`src/engine/`, JSON-RPC over stdio,
target `ananas-engine`) and the Cockpit UI (`ui/`, Tauri + React) that drives it.
Moonraker is the only print host, the G-code flavor is always `klipper`, there is
no cloud, no login, no telemetry and no update check. Keep it that way: no new
network endpoints, no vendor protocols.

## Build

Dependencies are built once per architecture into `deps/build/<arch>` (see
README). The slicer, engine and tests then configure against them:

```bash
# macOS (Ninja Multi-Config; the build scripts' own tree is build/<arch>)
cmake -S . -B build/dev -G "Ninja Multi-Config" -DORCA_TOOLS=ON -DBUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DCMAKE_IGNORE_PREFIX_PATH="/opt/local:/usr/local:/opt/homebrew" \
  -DDEP_BUILD_DIR="$PWD/deps/build/arm64"
cmake --build build/dev --config Release            # AnanasSlicer, ananas-engine, tests

# Linux / Windows
./build_linux.sh -s      # or build_win.bat -s; -d builds the deps first
```

The packaged route (DMG, AppImage, installer) is `build_release_macos.sh -sx`,
`build_linux.sh -i` and `build_win.bat`; CI runs them in `.github/workflows/build_all.yml`.

## Tests

```bash
cd build/dev && ctest -C Release --output-on-failure        # Catch2 suites and the engine tests
ctest -C Release -L Engine                                   # engine only
ctest --test-dir ./tests/libslic3r -C Release                # one suite
cd ui && npm ci && npm test && npm run typecheck             # Cockpit unit tests (Vitest)
cd ui && npm run e2e                                         # headless Chrome against the dev bridge and the engine
cargo test --manifest-path ui/src-tauri/Cargo.toml           # Tauri shell
```

Tests labelled `RequiresApp` need the built engine; `Docker` tests need the
virtual printer: `tests/engine/rig/rig.sh up` starts Klipper + Moonraker on
`127.0.0.1:17125` for the engine, `tests/virtual-printer/run.sh` is the rig for
the classic app. Never point a test at a real printer. Catch2 conventions and
where a new test belongs: [tests/AGENTS.md](tests/AGENTS.md).

## Documentation

- Docs live in `docs/`; the high-level design of a subsystem goes in `docs/HLSD/<subsystem>.md`
  (`engine.md`, `ui.md`, `stress-validierung.md`, `klipper-motion-control.md`, …).
- Describe the design as it stands — what the subsystem does, why it exists, and the constraints that shape it. Not the route that got there: no phases, task lists, status markers, or "before/after this PR" framing.
- Planning and investigation output stays in `docs/superpowers/`, which is gitignored. Never commit it.
- Write a doc only when the design is not evident from the code, and when a change invalidates an existing one, update it in the same PR.
- User documentation is the wiki (separate repository, English and German). In-app help links point there.
- Release notes come from `CHANGELOG.md` (Keep a Changelog); a user-visible change adds a line under `Unreleased`.

## Code style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables.
- `#pragma once` for headers. Smart pointers and RAII preferred.
- Parallelization via TBB — be mindful of shared state.
- Always use `SetSizerAndFit(sizer)` instead of `SetSizer(sizer)` on a top level window. Unless `SetSizer` must be called before the full layout is built; call `sizer->SetSizeHints(window)` afterwards in this case.
- Cockpit: TypeScript strict, React function components, no new UI library without a reason; design tokens in `docs/design/tokens.css`, rules in `docs/design/BRAND.md` (every value with its unit, every limit with its source key, no state by colour alone).

## Key entry points

- App startup: `src/OrcaSlicer.cpp`; GUI: `src/slic3r/GUI/`
- Slicing pipeline: `src/libslic3r/Print.cpp`; core algorithms: `src/libslic3r/` (GCode/, Fill/, Support/, Geometry/, Format/, Arachne/)
- All print/printer/material settings: `src/libslic3r/PrintConfig.cpp`
- Klipper limits import: `src/libslic3r/KlipperLimits.cpp`; Klipper time model: `src/libslic3r/GCode/KlipperMotionModel.cpp`, `docs/HLSD/klipper-motion-control.md`
- Engine: `src/engine/Engine.cpp` (method table and dispatcher), `src/engine/Stress.cpp` (force distribution), `docs/HLSD/engine.md`
- Cockpit: `ui/src/App.tsx`, `ui/src/session.ts`, `ui/src-tauri/src/main.rs`, `docs/HLSD/ui.md`
- Printer profiles: `resources/profiles/<vendor>.json`; the evidence every shipped printer needs: `docs/ananas-slicer/printer-firmware.json`
- Version: `version.inc` (`ANANAS_VERSION` is what users see; `SoftFever_VERSION` stays for the G-code producer line Moonraker's metadata parser keys on)

## Critical constraints

- **Backward compatibility required** for .3mf project files and printer profiles.
- **Cross-platform** — all changes must work on Windows, macOS, and Linux.
- Profile/format changes need version migration handling.
- Dependencies built separately in `deps/build/`, then linked to main app.
- **Klipper only, Moonraker only.** A change that needs another firmware, host or a cloud service is out of scope.
- The engine answers one request at a time; the Cockpit must stay responsive around long slices and analyses.

## Code review focus areas

- Changes must not cause regressions in existing functionality, defaults, profiles, or project compatibility.
- Features gated by options must not affect existing behavior when those options are disabled.
- Changes should follow the existing code style and architecture. Architectural changes should be justified in code comments and the PR description.
- Add helper functions or utilities only when existing code cannot reasonably be reused. Avoid duplication.
- Keep code concise and clear; simplify generated code by hand before review.
- Include targeted tests or documented verification for behavior changes, especially in slicing logic, G-code, profiles, formats, the engine protocol and GUI defaults.
- For profile changes (`resources/profiles/<Vendor>/**`), check that `version` in the sibling `resources/profiles/<Vendor>.json` was bumped and that the printer has an evidence record.
- Commit messages: `area: what changed` in the imperative (`gcode:`, `gui:`, `engine:`, `ui:`, `profiles:`, `ci:`), body says why; one change per commit; no trailers.

## Localization & translations

Catalogs live in `localization/i18n/<lang>/OrcaSlicer_<lang>.po` (the file names
are inherited; the gettext domain is fixed and independent of the app key); the
template is `OrcaSlicer.pot`.

### Terminology

- Keep recurring terms consistent within a language, and keep untranslated what must stay in English: brand and product names, acronyms, materials, file formats, G-code tokens, macros, variables and identifiers.
- Translate the *meaning*, not the words. Check what the string actually controls before translating it — English reuses one word for different things. `Flow ratio` (multiplier), `Flow Rate` (throughput) and `Flow Dynamics` (pressure compensation) are three different terms; `extruder` may mean the toolhead, the feeder motor, or the nozzle depending on the string.
- Reuse one template per recurring message shape (`Failed to connect to …`, `Are you sure you want to …?`), even where the English wording varies.

### Editing rules

- Only edit `msgstr` — **never** change `msgid`, and never "fix" wrong English in the translation alone. Report the source string instead.
- Preserve exactly: placeholders (`%s`, `%d`, `%1%`, `%zu`, `%%`), every `\n` (count *and* position, including leading/trailing), leading/trailing spaces, HTML tags, `℃`, and the file's encoding and line endings.
- **Never reorder positional arguments** in a `c-format` string. If the msgid is `%d` then `%s`, that order must hold — swapping them breaks at runtime.
- `msgctxt` separates homonyms — always read it. `Back`/`Camera View` is the rear view of the 3D navigator, while `Back`/`Navigation` is the go-back button; `Top` exists in the *Alignment*, *Layers* and *Camera View* senses.
- When a string needs disambiguating, add context in the source (`_L_CONTEXT`/`_u8L_CONTEXT`), don't work around it in the translation.
- A literal `%` inside a string xgettext flagged `possible-c-format` will fail `msgfmt`. Fix it with a `// xgettext:no-c-format, no-boost-format` comment above the string in the source — do not mangle the translation or use `%%` in text that is never passed through printf.
- Plural entries: read `nplurals` from the catalog's `Plural-Forms` header (it is **not** always 2 — ja/ko/zh/th/vi use 1, ru/cs/pl/lt use 3, uk uses 4). Each form must be genuinely inflected for its quantity; repeating one sentence across all forms is a bug in Slavic/Baltic languages, though it is correct for Turkish and Hungarian.
- An entry whose `msgstr` equals its `msgid` is untranslated even though it is not empty; a plural entry with any empty form is likewise incomplete.
- Mark machine-produced translations with an `# AI Translated` translator comment. Don't add it to a human translation you didn't actually rewrite.
- Don't reflow or re-wrap unrelated entries — keep the diff limited to the strings you changed.

### Verifying

- `scripts/run_gettext.bat --full` (Windows) regenerates the template, merges every catalog and compiles the `.mo` files. It must exit 0.
- Or check a single catalog with `msgfmt --check-format -o <out>.mo localization/i18n/<lang>/OrcaSlicer_<lang>.po`.
- Fuzzy entries are not shown to users. If you correct one, clear its `fuzzy` flag, otherwise the fix never ships.
