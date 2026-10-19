# Filament IDs (`filament_id`)

`filament_id` identifies one **filament product**: one named spool product = one id, shared by
all of that product's per-printer / per-nozzle variants, in every profile bundle that ships it.
Devices use it to match a physical spool or tray to a filament preset. It is never per-color,
per-printer, per-nozzle, or per-preset (per-preset identity is `setting_id`), and it is never
per-bundle either — PolyLite PLA carries the same id whether the preset lives in the
OrcaFilamentLibrary (OFL), Qidi, or Snapmaker bundle. The granularity is the name on the spool,
not the brand behind it: `AAA PLA Lite` and `AAA PLA Pro` are two filaments with two ids, not
variants of one.

**How it is generated:** an id is computed, never invented. `scripts/orca_profile_tool.py`
mints it as a deterministic hash of the product's identity — the triple
`(filament_vendor, filament_type, filament name)`, where the filament name is the preset name
with its `@...` variant suffix stripped — producing an 8-character `OF*` code that is the
same for that product in every bundle, in every PR, on every machine. For example, Polymaker's
PolyLite PLA presets (`PolyLite PLA @base`, `PolyLite PLA@Q2-Series`, …) resolve
`filament_vendor` `Polymaker`, `filament_type` `PLA`, and filament name `PolyLite PLA`; hashing
`filament_product/Polymaker/PLA/PolyLite PLA` yields `OF5CgdDq`, and that is the id the
OrcaFilamentLibrary, OrcaArena, Qidi, and Snapmaker bundles all arrive at independently
(derivation details in the Minting section).

**How it is used:** at runtime the id is the join key between hardware and profiles.
When a printer reports what a tray holds (Qidi box, Creality CFS, Klipper, Snapmaker),
ANANAS SLICER matches the reported id against the filament presets
compatible with that printer to select the right profile; other features — tray display
names, support-material detection, vitrification warnings, multi-nozzle filament grouping —
look up material properties by id alone. An id that changes is not forwarded anywhere: a
tray or record still holding the old value falls back to matching by material type until the
user re-selects the filament, so identity changes are made deliberately and rarely.

This page is the rule for authoring `filament_id` in system profiles
(`resources/profiles/**`). CI enforces everything below; the short version is:

> [!IMPORTANT]
> **Never write a `filament_id` value by hand.** A new filament gets its id from
> `python scripts/orca_profile_tool.py generate-id`; one already in the tree has one — inherit it.

## The design

Because several consumers match **globally by id alone, first hit wins** (see the next
section), any two materials sharing one id feed wrong data somewhere — a wrong tray name, a
wrong support-material flag, a wrong nozzle grouping — and inside one printer a duplicated id
makes AMS spool matching a coin toss. Hand-written ids produce such collisions constantly, so
the system is built to make them impossible: an id is a pure hash of the product's identity —
no registry to maintain, no next-free-number ceremony, no way for two concurrent PRs to race
for the same number, and no way to get it wrong by hand, because you never write it by hand.
CI holds every id in the tree to that rule, so the profiles themselves are the whole record of
which products exist and which bundles ship them.

## Who consumes the id

The canonical consumer is tray-to-preset matching: a device reports a tray material id
(`tray_info_idx`), and the shared matching pipeline (`PresetBundle::sync_ams_list` and
friends) resolves it to a preset. The matcher is printer-scoped and first-match-wins:
scanning only compatible root presets — system roots plus user-made custom filaments, which
are user roots carrying their own `P*` ids; a preset derived from another resolves through
its root and never matches directly — it picks the first one whose `filament_id` equals the
tray's. On a miss it falls back by filament type: a system `Generic <type>` preset
(matched by name, then by type similarity), else the slot's previous selection, else any compatible system generic or,
failing that, any compatible system preset, else the slot is skipped — every fallback
selection surfaces a user-visible notice.

No device integration follows this pattern end to end yet. Every agent still synthesizes a
preset id client-side (by type, brand, or color lookups against the loaded presets) before
the pipeline runs; they are intended to converge on the same pattern, with the
device-reported tray material id flowing through the shared matcher.

| Ecosystem | Where the tray id comes from today |
| --- | --- |
| Qidi box | composed at runtime as `QD_<series>_<vendor>_<typeidx>` — vendor and type indices from the device's per-slot saved variables, the series digit inferred client-side from the printer model/name. No preset carries a `QD_*` value, so the slot currently resolves by filament type; mapping the composed id onto the filament's minted id belongs in the agent |
| Creality CFS | through the Moonraker agent: runtime lookup by filament type |
| Klipper (AFC / Happy Hare) | runtime lookup by filament type |
| Snapmaker | runtime color/vendor/type match |

Tray-to-preset matching is printer-scoped, but **several consumers match globally by id alone,
first hit wins**: tray display names, `filament_is_support`, vitrification warnings, and
multi-nozzle filament grouping in the slicing pipeline (`FilamentGroup::try_merge_filaments`
merges plate slots sharing one `(filament_id, color)` pair, with matching
extruder-printability, onto one nozzle group; the engine is implemented but no grouping path
calls it yet).
Two *different* materials sharing one id
feed wrong data to those consumers even when the presets live in different vendors — so
cross-material id sharing is never safe. Within one printer, duplicate ids break AMS matching:
the matcher picks whichever preset loads first (it now logs an "Ambiguous AMS filament match"
warning, but the pick is still arbitrary) and the tray-edit dialog, which lists one entry per
id, hides the second preset entirely. The profile validator's `-f` check
(`check_filament_subtypes` → `PresetBundle::check_duplicate_filament_subtypes`) rejects this
per printer, and CI runs it tree-wide.

Two more consumer-side facts worth knowing:

- The machine-facing dialogs (AMS tray edit, AMS dry control, calibration history, extrusion
  calibration) offer the filaments a connected printer can use by the same compatibility rule
  the plater uses (an empty `compatible_printers` means *every* printer). Alias shadowing
  still applies: a vendor's same-name profile supersedes the library generic. That is what
  puts Orca Filament Library materials in those lists — deduplicated to one entry per
  `filament_id` in the AMS and calibration-history dialogs, while extrusion calibration
  deliberately lists every matching preset by full name.
- The id is load-bearing at startup: an instantiated system filament (one marked
  `"instantiation": "true"` — see the structure rules) that resolves **no**
  `filament_id` anywhere in its `inherits` chain is a hard load error in the C++ loader
  (`Can not find filament_id for <name>`) that discards the entire vendor bundle (for the
  OrcaFilamentLibrary itself the failure is messier: library presets loaded before the
  failing one survive, and every vendor bundle whose filaments inherit from the library is
  then discarded for want of a base). CI's structure check catches this before it ships.

## Do I need a new id? The one-question test

> **Would a user consider this a different spool product than anything already in the tree?**

Different polymer, different sub-brand (Basic / Matte / Silk / HF), fiber-filled sibling, or a
second selectable diameter → **new filament, new id**. The same spool tuned for another printer
or nozzle → **join the existing filament** (keep its base name and inherit it; no id
key needed). Tuning a generic material → **join the OrcaFilamentLibrary filament** (inherit
`Generic X @System` and keep the `Generic X` base name; no id key needed).

| Situation | id |
| --- | --- |
| Per-printer / per-nozzle variant of an existing material | same id (inherit it) |
| Sub-brand or product line (PLA vs PLA Matte vs PLA Silk vs PLA HF) | new id each |
| Color | never a new id |
| Second diameter of the same product (1.75 + 2.85) | sibling filament, new id |
| "High-speed" tuned for a *different printer model* | same id (it is a printer variant) |
| "High-speed" selectable *alongside* the normal preset on one printer | new name, so a new id (it is a product line) |

## Structure rules

1. **Every preset carries the id of its own product, wherever it gets it from.** The id is a
   function of the preset's own triple (rule 5), and `inherits` carries settings, never
   identity. So a preset may declare the key itself or inherit it from any ancestor — a
   `<Filament> @base` root, a real (instantiated) preset of the same filament, an
   OrcaFilamentLibrary preset — and CI checks one thing: the id it ends up with equals the
   mint of *its* triple. The usual shape is one `@base` root (`"instantiation": "false"`)
   declaring the key and the per-printer variants inheriting it; a filament may have several
   roots — Qidi's PolyLite PLA has four per-series roots (`PolyLite PLA@Q2-Series`,
   `@Q2C-Series`, `@X-Max 4-Series`, `@X-Plus 5-Series`) — which then all declare the identical
   id. A branded filament that borrows a generic's settings (`Flashforge ABS Basic @FF C5`
   inherits `Generic ABS @System`) declares its own id, because its triple is its own.
2. **The filament name is the base name**: the preset name with everything from the first
   (optionally space-preceded) `@` stripped. `MyBrand PLA @Orca 3D Fuse1` and `MyBrand PLA@HS`
   are both the filament `MyBrand PLA`.
3. **Within one filament, variants' `compatible_printers` are pairwise disjoint** — per printer,
   at most one compatible instantiated preset per id, or AMS matching turns ambiguous. The
   C++ validator's `-f` check enforces this, tree-wide in CI. Since one product carries one id
   and cannot be split onto two, this rule is the *only* remedy for such an ambiguity: narrow
   the `compatible_printers`, or retire the preset that duplicates another.
4. **Generics belong to OrcaFilamentLibrary.** A vendor tuning a generic material inherits
   `Generic X @System`, keeps the `Generic X` base name (that alias is what hides the library
   preset on your printers, and it is what makes its triple — and so its id — the library's)
   and sets a non-empty `compatible_printers` — e.g. `Generic PLA @Sovol SV08 MAX` inherits
   `Generic PLA @System` and lists three Sovol nozzles. Renaming such a preset makes it a
   different product by rule 5, so it then needs its own id.
5. **Ids follow the product identity.** The id is a pure function of the product triple
   `(filament_vendor, filament_type, filament name)`, so correcting any of them re-mints the id
   **by design**, applied by `generate-id` (preview with `--dry-run`, confine with `--vendor`);
   the exact sequence is in the FAQ. Nothing forwards
   the old value, so anything outside the tree that stored it — a device tray, a calibration
   record, a saved project — falls back to matching by filament type until the user re-selects
   the filament. Re-mint deliberately, and only to fix a genuinely wrong identity.
   (`renamed_from` still gates preset-*name* compatibility, as before.)

## Minting — nobody invents ids

New ids are deterministic, computed exactly like the `setting_id` precedent
(the `setting_id` half of `scripts/orca_profile_tool.py generate-id`):

```text
FILAMENT_ID_NAMESPACE = uuid5(setting-id NAMESPACE, "filament_id")
                      = c4d3ff49-4c32-5534-a3e3-00894157ab97
filament_id = "OF" + base62_6( uuid5(FILAMENT_ID_NAMESPACE,
                  "filament_product/<filament_vendor>/<filament_type>/<filament_name>") )
```

`base62_6` is the low 6 base62 digits (alphabet `0-9A-Za-z`) of the UUID taken as a big-endian
integer, most-significant digit first; with the `OF` prefix the full id is 8 chars, within the
AMS length limit. The triple comes from the root preset's *flattened* config:
`<filament_vendor>` is the filament
**manufacturer** (`"Polymaker"`, or `"Generic"` for generics — never the printer brand),
`<filament_type>` the material type, `<filament_name>` the root's base name; the two config
values are inheritable list options and the first element counts.

Content-addressing on that triple is what makes the whole system converge. The key contains no
bundle name, so the same product mints the same id in every bundle — moving a filament into
OrcaFilamentLibrary never changes its id, and two vendors independently shipping the same
product arrive at the same id without coordinating. `Polymaker/PLA/PolyLite PLA` mints
`OF5CgdDq`, and that one id is declared by the OrcaFilamentLibrary, OrcaArena, Qidi, and
Snapmaker bundles alike; the OFL generic `Generic/PLA/Generic PLA` mints `OFDSrzZ8`, claimed
by 35 bundles — most by independent declarations converging on the same mint, the rest
purely through inheritance from the OFL preset.

Nothing but the triple feeds the mint — not the rest of the tree, not what another preset of
the product happens to carry. Determined triple, determined id: one product
carries one id and there is no second acceptable value for it, so any other value on a preset
is a mismatch `check` reports and `generate-id` pulls back. Two *different* products whose
triples mint the same base62 value would be a collision (a roughly 36-bit id space against a
few thousand products); nothing salts past it: `check` reports it naming both products,
`generate-id` refuses to write it, and the remedy is a rename so their triples differ. Where
two presets of one product would be AMS-ambiguous on a printer, the fix is likewise in the
profiles — make their `compatible_printers` disjoint (structure rule 3), retire the redundant
preset, or, if they really are different products, give them different names so their triples
differ. Never a second id for one triple.

Workflow for a new filament:

```bash
# 1. Author the filament with NO filament_id key anywhere.
python scripts/orca_profile_tool.py generate-id --dry-run  # 2. preview the ids — writes nothing
python scripts/orca_profile_tool.py generate-id           # 3. apply them to the profile file(s)
python scripts/orca_profile_tool.py check                 # 4. validate — everything CI checks
```

`generate-id` makes every filament's id equal the mint of its own
`(filament_vendor, filament_type, filament name)` triple: it inserts one where an instantiated
filament resolves none, and re-derives one that does not match. A preset that *inherits* a
mismatching id is the one case left to the author — check 2b names it, and the fix is to inherit
a preset of the same filament or to give the preset its own key. A declaration is left alone
exactly when it already equals the one id its triple mints, and a collision (check 2d) is
reported and left unwritten. The same run assigns
`generate_preset_setting_id(vendor, type, name)` to every instantiated filament, process
and machine preset of every vendor except BBL, which keeps its authoritative `G*` ids, strips
`setting_id` from base profiles, and fixes the misspelled `settings_id` key — dropped, or, for
BBL, whose ids have no formula to fall back on, restored under the correct name. It is idempotent and
byte-preserving (indentation, BOM, and line endings intact, every edited file re-parsed to fail
loudly), and a no-op on a tree that already passes `check`.

- `--filament-id` limits the run to `filament_id`.
- `--setting-id` limits the run to `setting_id`. The two exclude each other; pass neither to
  write both.
- `--vendor VENDOR` confines the run to that bundle; repeatable. The id is a function of the
  triple alone, so a narrowed run writes exactly what a full one would; `check` reports
  whatever it left outside.
- `--dry-run` reports what the run would do and writes nothing, so
  `generate-id --dry-run --vendor <Vendor>` previews just that bundle.
- `--profiles DIR` points the tooling at a different profile tree (default
  `resources/profiles`).

The tool's other commands maintain the tree around the ids: `fix` normalises profile files,
`trim` drops files no `<vendor>.json` list references, and `update-index` rebuilds those lists.
They do not touch ids; `--help` documents them.

**Identity fixes need no separate command.** `generate-id` re-derives an id that no longer matches
its triple exactly the way it fills in a missing one, so a rename or a `filament_vendor` /
`filament_type` correction is just: fix the config and run `generate-id` (confine it with
`--vendor`, preview it with `--dry-run`).

If you skip the tooling, CI fails and prints the remedy: the expected id for your filament and
the instruction to run `python scripts/orca_profile_tool.py generate-id`.

## Ids other systems compose

Every filament profile carries a minted id, with no exceptions and no spellings held back for
anyone. There is therefore no reserved namespace to respect and no bundle that owns one: an id
some other system composes for its own purposes is simply not the mint of a triple, so it
cannot be a system profile's `filament_id`, and the format check rejects it for that reason
alone — same error, same remedy, whoever wrote it.

Three such spaces exist around us, and are worth recognising so nobody mistakes one for an id
to copy into a profile:

- **Bambu's `GF*` catalog.** No shipped bundle targets a Bambu printer, but two foreign values
  from Bambu's device catalog survive in the tree by design: `GFS00` / `GFS01` in
  `DynamicPrintConfig::get_filament_type` (the support display type, see
  [Ids at the printer boundary](#ids-at-the-printer-boundary)) and the entries of
  `resources/profiles/blacklist.json`. Neither is a `filament_id`, and the rule is about
  `filament_id` and nothing else.
- **Qidi's `QD_*` protocol ids.** The Qidi box composes `QD_<series>_<vendor>_<typeidx>` at
  runtime (slot vendor and type indices reported by the device, the series digit inferred
  client-side from the printer model/name). Qidi presets carry ordinary minted `OF*` ids
  (generics share the OFL ids), so a composed id matches no preset and the slot falls back to
  filament type; translating it to the filament's id belongs in `QidiPrinterAgent`. Treating
  per-series protocol ids as preset ids would put one product under five ids
  (`QIDI PLA Rapido` would be `QD_0_1_1` through `QD_4_1_1`) — exactly the fragmentation the
  mint rule removes.
- **`P` + 7 hex chars, and `"null"`.** What `CreatePresetsDialog.cpp` gives a filament a *user*
  creates. Those are user presets, not system profiles, and the two never meet in the tree.

## Ids at the printer boundary

The printer never sees an id of ours and we never store one of its. The only place an id
crosses from a printer into the app is the sidebar's filament sync (`Sidebar::sync_ams_list`),
which reads the filament changer through `src/slic3r/Utils/MoonrakerFilaments.hpp`: Moonraker's
`lane_data` namespace (AFC, recent Happy Hare), Happy Hare's `mmu` object, a Qidi box or a
Snapmaker toolhead report materials, colours and vendors, never preset ids, and
`MoonrakerFilaments::resolve_filament_ids` picks the preset for every loaded slot (the vendor's
closest colour match when the changer names a vendor, else a visible system base preset of that
material, else `UNKNOWN_FILAMENT_ID`, which the sync turns into a generic preset of that type).
A composed protocol id such as Qidi's `QD_*` is therefore never looked up as a `filament_id`.

One libslic3r site still names a foreign catalog id, and needs no change:
`DynamicPrintConfig::get_filament_type` in `PrintConfig.cpp` picks `PLA-S` / `Sup.PLA` and
`PA-S` / `Sup.PA` for a support filament by testing `filament_id` against `GFS00` and
`GFS01`, and otherwise falls back on `filament_type` — a fallback that returns those same two
pairs for `"PLA"` and `"PA"`. With minted `OF` ids the fallback produces exactly what the id
branches produced. (The only config that ever carries a singular `filament_id` key is the AMS
tray config built in `Plater.cpp`, and that one never reaches this function.)

## How CI enforces this

Profile CI (`check_profiles.yml`) runs `check_filament_ids()` tree-wide via
`scripts/orca_profile_tool.py check`. Every check judges the tree against the rules on this
page and nothing else — there is no recorded id state to match and no grandfather list of any
kind.

The checks, in brief:

- **Format** — every id occurring in the tree is `OF` + 6 base62 chars. No exceptions.
- **Identity** — the id is a function of the triple alone. A declared `OF*` id must equal the
  one id its declarer's own triple mints, with no second acceptable value; the id an
  instantiated preset *inherits* must equal the mint of *its* own triple, however it inherits
  it (a root, a real filament, a library preset — structure rule 1); and every instantiated
  system filament must resolve an effective id at all (recall: an id-less one is a hard load
  error in C++ that discards the whole vendor bundle); and no two products mint one id (a
  base62 collision, resolved by renaming one of them). The errors print the expected id.
- **Triple integrity** — every declarer must resolve a non-empty `filament_vendor` and
  `filament_type` (generics use `"Generic"`), and all declarers of one filament within a
  bundle must agree on the triple.

A profile that declares an id no triple mints — a foreign vendor catalog id, a composed Qidi
one, a hand-typed value, whatever its vendor — fails the format check. Two products sharing one
id are caught by the identity check whether the id is declared or inherited.

The same `check` run holds every declared id to the AMS 8-character limit, tree-wide and for
every vendor alike, scoped to the presets a vendor's index actually references (a file the index
never loads cannot break AMS matching).

Complementing the Python checks, CI also runs the C++ profile validator with `-f`
(`check_filament_subtypes`): it loads the bundle exactly as the app does and flags any printer
for which two or more compatible filament presets share one `filament_id` — the runtime-shaped
ambiguity check behind structure rule 3.

## FAQ

- **A new color of an existing product?** Never a new id — colors are not filaments.
- **A second diameter (1.75 mm and 2.85 mm) of the same product?** A sibling filament with its
  own id: two diameters are separately selectable spool products.
- **A high-speed tune of an existing material for another printer model?** Same filament:
  keep the base name and inherit its root; no id key needed.
- **A tuned generic ("our profile for Generic PLA")?** Inherit `Generic PLA @System`, keep the
  `Generic PLA` base name, set `compatible_printers`; no id key needed.
- **A branded filament that borrows a generic's settings?** Fine — inherit `Generic X @System`
  (or any real filament) for the settings and declare the id of your own filament; run
  `python scripts/orca_profile_tool.py generate-id` to mint it. Inheritance never changes the id.
- **I need to fix a filament's `filament_vendor` or `filament_type`.** Fix the config, run
  `generate-id --vendor <Vendor>` (preview with `--dry-run`), and commit the result. The id
  re-derives from the corrected identity, and
  nothing forwards the old value, so a tray or record still holding it falls back to matching by
  filament type.
- **I need to rename a filament.** Rename the presets (adding `renamed_from`, which keeps the
  preset *name* resolving), then `generate-id --vendor <Vendor>` (preview with `--dry-run`). The
  id follows the new filament name; as with any identity fix, the old id
  is not forwarded.
- **Can I reuse a `QD_*` id for a Qidi profile?** No — it is not a mint, so it is not a
  `filament_id`. Those values are composed by the box at runtime, and no preset carries one.
  Author Qidi filaments like any other vendor's.
- **CI says my filament needs an id.** Run `python scripts/orca_profile_tool.py generate-id` and
  commit the result. Do not type an id by hand.

For general profile authoring, see the profile development guide on the
[OrcaSlicer wiki](https://www.orcaslicer.com/wiki).
