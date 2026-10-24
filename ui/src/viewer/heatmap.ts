// The heatmap ramp of docs/design (BRAND.md 3.3): eleven bins from blue over neutral to yellow, luminance
// rising monotonically so it reads in greyscale. Values above a machine limit get their own colour.

export const RAMP: readonly string[] = [
  "#4b62a8",
  "#5a6ea6",
  "#6a7aa2",
  "#7b879e",
  "#8d9599",
  "#a0a393",
  "#b3b18a",
  "#c6bf7e",
  "#d9cd6e",
  "#ebdb5c",
  "#fbe84a",
];

/** Out of range: the limit signal colour, never part of the ramp. */
export const OVER_COLOR = "#ff6f5e";
export const TRAVEL_COLOR = "#5c6b77";

export type Quantity = "speed" | "flow" | "accel";

export const QUANTITIES: readonly { id: Quantity; label: string; unit: string }[] = [
  { id: "speed", label: "Speed", unit: "mm/s" },
  { id: "flow", label: "Flow", unit: "mm³/s" },
  { id: "accel", label: "Accel", unit: "mm/s²" },
];

export interface Scale {
  /** Range of the ramp. A value above `limit` (when there is one) is out of range, not at the top of the ramp. */
  min: number;
  max: number;
  limit: number | null;
}

/** The bin 0..10 a value falls in, or -1 for a value above the limit. A flat range puts everything in the middle. */
export function binOf(value: number, scale: Scale): number {
  if (scale.limit !== null && value > scale.limit) return -1;
  const top = scale.limit !== null ? Math.min(scale.max, scale.limit) : scale.max;
  if (top - scale.min < 1e-9) return 5;
  const ratio = (value - scale.min) / (top - scale.min);
  return Math.min(RAMP.length - 1, Math.max(0, Math.floor(ratio * RAMP.length)));
}

export interface LegendBin {
  from: number;
  to: number;
  color: string;
}

export function legendBins(scale: Scale): LegendBin[] {
  const top = scale.limit !== null ? Math.min(scale.max, scale.limit) : scale.max;
  const step = (top - scale.min) / RAMP.length;
  return RAMP.map((color, index) => ({ from: scale.min + step * index, to: scale.min + step * (index + 1), color }));
}

/** How many of `values` fall above the scale's limit. */
export function countOver(values: ArrayLike<number>, scale: Scale): number {
  if (scale.limit === null) return 0;
  let count = 0;
  for (let i = 0; i < values.length; i += 1) if (values[i]! > scale.limit) count += 1;
  return count;
}
