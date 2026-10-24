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
