// Which settings the cockpit shows, how they are grouped and where each row's limit comes from.
// The values, bounds, labels and units come from the engine; this table decides only the layout
// and the step size of a row.

import type { EffectiveLimits, KlipperLimitValues, ResolveResult, SchemaOption } from "../engine/protocol.ts";
import type { CockpitGroup, CockpitParam, LimitKind } from "./model.ts";

interface RowSpec {
  key: string;
  /** The row's own name: the engine's label is written for the settings tabs and is ambiguous in a table. */
  label: string;
  step: number;
  decimals: number;
}

interface GroupSpec {
  id: string;
  title: string;
  /** The Klipper limit every row of the group is held against. */
  limit: { field: keyof KlipperLimitValues; source: string; kind: LimitKind };
  rows: RowSpec[];
}

const speed = ([key, label]: readonly [string, string]): RowSpec => ({ key, label, step: 5, decimals: 0 });
const accel = ([key, label]: readonly [string, string]): RowSpec => ({ key, label, step: 100, decimals: 0 });

export const GROUP_SPECS: readonly GroupSpec[] = [
  {
    id: "speed",
    title: "Speed",
    limit: { field: "max_velocity", source: "printer.max_velocity", kind: "machine" },
    rows: (
      [
        ["outer_wall_speed", "Outer wall"],
        ["inner_wall_speed", "Inner wall"],
        ["small_perimeter_speed", "Small perimeters"],
        ["sparse_infill_speed", "Sparse infill"],
        ["internal_solid_infill_speed", "Internal solid infill"],
        ["top_surface_speed", "Top surface"],
        ["bridge_speed", "Bridge"],
        ["gap_infill_speed", "Gap infill"],
        ["support_speed", "Support"],
        ["initial_layer_speed", "First layer"],
        ["travel_speed", "Travel"],
      ] as const
    ).map(speed),
  },
  {
    id: "acceleration",
    title: "Acceleration",
    limit: { field: "max_accel", source: "printer.max_accel", kind: "machine" },
    rows: (
      [
        ["default_acceleration", "Default"],
        ["outer_wall_acceleration", "Outer wall"],
        ["inner_wall_acceleration", "Inner wall"],
        ["top_surface_acceleration", "Top surface"],
        ["bridge_acceleration", "Bridge"],
        ["initial_layer_acceleration", "First layer"],
      ] as const
    ).map(accel),
  },
  {
    id: "pressure-advance",
    title: "Pressure advance",
    // A slicer override of the calibrated value is legitimate, so the limit is a reference, not a bound.
    limit: { field: "pressure_advance", source: "extruder.pressure_advance", kind: "reference" },
    rows: [{ key: "pressure_advance", label: "Pressure advance", step: 0.001, decimals: 3 }],
  },
];

/** The first element of a serialized per-extruder value, as a number; NaN if it is not one. */
export function firstNumber(serialized: string | undefined): number {
  if (serialized === undefined) return Number.NaN;
  const first = serialized.split(",")[0]?.trim() ?? "";
  return first === "" ? Number.NaN : Number(first);
}

const FALLBACK_MAX = 1_000_000;

function row(spec: RowSpec, group: GroupSpec, option: SchemaOption, value: number, limits: EffectiveLimits): CockpitParam {
  // A reference is the printer.cfg value; without an imported printer.cfg or Moonraker snapshot there is none.
  const hasReference = group.limit.kind !== "reference" || limits.source !== "profile";
  return {
    id: spec.key,
    label: spec.label,
    unit: option.unit,
    value,
    step: spec.step,
    decimals: spec.decimals,
    min: option.min ?? 0,
    max: option.max ?? FALLBACK_MAX,
    limit: hasReference
      ? { value: limits.values[group.limit.field], source: group.limit.source, kind: group.limit.kind }
      : null,
  };
}

/**
 * The cockpit groups for a resolved config. Rows whose setting is missing from the schema or the config are
 * left out, and without effective limits there is nothing to hold a row against, so there are no groups.
 */
export function buildGroups(schema: ReadonlyMap<string, SchemaOption>, resolved: ResolveResult): CockpitGroup[] {
  const limits = resolved.limits;
  if (limits === null) return [];
  const groups: CockpitGroup[] = [];
  for (const group of GROUP_SPECS) {
    const params: CockpitParam[] = [];
    for (const spec of group.rows) {
      const option = schema.get(spec.key);
      const value = firstNumber(resolved.config[spec.key]);
      if (option !== undefined && !Number.isNaN(value)) params.push(row(spec, group, option, value, limits));
    }
    if (params.length > 0) groups.push({ id: group.id, title: group.title, params });
  }
  return groups;
}
