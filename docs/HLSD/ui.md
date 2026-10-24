# User interface

`ui/` is the ANANAS SLICER user interface: a React front end (Vite, TypeScript, Tailwind) in a Tauri shell. It owns
no slicing knowledge. Every setting, bound, limit and preset comes from `ananas-engine` (see `engine.md`), and the
look follows `docs/design/BRAND.md` and its tokens.

## Layers

| Layer | Where | Role |
|---|---|---|
| Shell | `ui/src-tauri` | Starts the engine as a child process, forwards requests (`engine_call`), turns the engine's notifications into `engine-notification` events. Closing the app ends the engine. |
| Transport | `ui/src/engine/transport*.ts` | One interface, three carriers: the Tauri shell, the HTTP dev bridge (a plain browser), a child process (tests). |
| Client | `ui/src/engine/client.ts` | Typed methods over a transport. |
| Session | `ui/src/state/session.ts` | The cockpit's state without a UI framework: selection, overrides, imported limits, resolved config, running slice. Requests run one after another, so an older answer never replaces a newer one. |
| Cockpit | `ui/src/cockpit` | The parameter table: rows, groups, limit states. `model.ts` holds the pure rules, `params.ts` the table of which settings are shown and where each limit comes from. |

## Rows and limits

A row is `label | value + unit | - + | limit` (BRAND.md 5.2). The value is held against the effective Klipper limit
the engine reports for the config: the imported `printer.cfg` or Moonraker snapshot when there is one, otherwise the
profile's machine limits; the source is shown in the header. Speeds are held against `max_velocity`, accelerations
against `max_accel`. A row's own name is set in `params.ts`, because the engine's labels are written for the
settings tabs and are ambiguous in a table.

- A value is never clamped. Input outside the schema's bounds, or an input the engine refuses, is marked `INVALID`
  and stays visible until the user corrects or reverts it (Esc).
- A group shows its violation count when collapsed.
- Pressure advance is a reference, not a bound: it shows the `printer.cfg` value only when one was imported, and
  an em dash otherwise.
- Slicing with a value above a limit first asks for an acknowledgement that lists every violation.

## The viewport

`ui/src/viewer` draws the bed, the model and the G-code. `ViewerScene.ts` is the only WebGL code; everything that can
be decided without a GPU sits in pure modules next to it (`bed`, `heatmap`, `inspect`, `objects`, `preview`) and is
tested without one.

- **Where an object stands** is decided by the engine (`model.bounds`), never by the browser: the browser builds
  its matrix from the engine's offset, rotation and scale (`T * Rz*Ry*Rx * S`), and a test compares it with the
  engine's own bounding box for rotated, scaled and multi-object models. Dragging moves the object at once and the
  engine confirms the placement when the pointer is released.
- **Looks:** soft shadows from a key light, a studio environment for the reflections, screen-space ambient occlusion,
  and crease edges so a part reads like a CAD model: smooth over curved surfaces, sharp across creases.
- **Views of the model:** *Solid*; *Mesh* (triangles as a wireframe); *Inspect*, which paints every face by what the
  printer has to do for it: no support needed, near the profile's support threshold, needs support, on the bed. The
  colours are flat and unlit so they are the legend's colours; the camera may go under the bed, which turns
  translucent, to look at the undersides. The mesh statistics (triangles, volume, open edges, repairs) are shown.
- **Exploded view:** the parts of a multi-part model move apart along the line from the model's centre; for a
  G-code the layers move apart by a gap per layer.
- **G-code:** the toolpaths come from `preview.*` in layer chunks, are coloured by speed, flow or acceleration with the
  eleven-bin ramp of the design, and a value above the machine limit takes the limit colour instead of the top of
  the ramp. A layer range and a travel-move switch limit what is drawn.
- Without WebGL the viewport says so; the cockpit keeps working.

Not built yet: drawing a path with its real width and height, cut-away sections, painting supports, arranging
several objects automatically.

## The printer

The way from a slice to a machine: connect, slice against the printer's own limits, store the G-code, print.

- **Connect** (Printer button, top bar): the address of a Moonraker or FLUXRAKER host and an optional API key. The key
  stays in memory and goes only to that address. When the host's Klipper is ready, its limits (the running Klipper's
  own parse of `printer.cfg`) replace the profile's: the cockpit holds every row against them and a speed the
  printer cannot reach turns red at once. Disconnecting goes back to the profile's limits.
- **Send to printer** stores the G-code. **Send and print** asks first, naming the printer, the file, the time and
  the filament, and prints nothing until it is confirmed; cancelling the question does nothing. The session never
  starts a print by itself.
- While a print runs the printer is read every two seconds: state, file and progress, with a Cancel print button.
- **Choose... / Save G-code...** are the desktop shell's native file dialogs. Saving copies a G-code the engine wrote
  to the chosen file through one narrow command (`export_gcode`): only a file in the engine's output folder, only a
  `.gcode` target, no links out of the folder. In a browser the model path field is used instead.

Nothing here has touched a real printer. The tests run against the virtual printer (below) only.

## Running it

```sh
cd ui
npm install
npm test                    # unit, session and app tests; most of them start the real engine
npm run e2e                 # the viewer in headless Chrome: page and pixels (needs Chrome; CHROME_BIN picks it)
cargo test --manifest-path src-tauri/Cargo.toml     # the Rust bridge against the real engine

# the way to the printer, against the virtual printer (real Klipper and Moonraker, reachable on 127.0.0.1 only)
tests/engine/rig/rig.sh up                          # then: export ANANAS_RIG_URL=http://127.0.0.1:17125
npm test                                            # the printer tests run now instead of being skipped
npm run e2e                                         # also drives the dialogs and a whole print
tests/engine/rig/rig.sh down

# in a plain browser
npm run bridge              # serves the engine on 127.0.0.1:4173 (localhost only)
npm run dev                 # http://127.0.0.1:1420

# as the desktop app
npx tauri dev
npx tauri build --debug --no-bundle
```

The engine binary and the profiles default to this worktree's build tree (`build/mini/...`,
`resources/profiles`); `ANANAS_ENGINE_BIN`, `ANANAS_PROFILES_DIR`, `ANANAS_VENDOR` (default `Voron`) and
`ANANAS_SAMPLE_MODEL` override them. The dev bridge is a development aid only: it has no authentication and writes
G-code wherever a request says, so it listens on localhost and the app never uses it.
