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
| `slice` | as `config.resolve`, plus `model` and `output` | the G-code path, estimated time, layer count, filament use, effective limits and violations; `slice.progress` notifications with the request id |

`process` and `filament` default to the printer's own. `overrides` maps a setting key to its serialized value.
`limits` is an object with the Klipper limits and a `source` of `printer_cfg` or `moonraker`; it replaces the
limits derived from the profile.

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
