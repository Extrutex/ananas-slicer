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

## Running it

```sh
cd ui
npm install
npm test                    # unit, session and app tests; the last two start the real engine
cargo test --manifest-path src-tauri/Cargo.toml     # the Rust bridge against the real engine

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
