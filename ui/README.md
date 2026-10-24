# ANANAS SLICER user interface

React cockpit in a Tauri shell, driving `ananas-engine`. Design and layers: [`docs/HLSD/ui.md`](../docs/HLSD/ui.md).

```sh
npm install
npm test                                              # needs the engine built (see docs/HLSD/engine.md)
cargo test --manifest-path src-tauri/Cargo.toml
npm run bridge & npm run dev                          # in a browser: http://127.0.0.1:1420
npx tauri dev                                         # as the desktop app
```
