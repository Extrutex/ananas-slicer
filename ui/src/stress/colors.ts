// Farben der Kraftverteilung. Die Rampe ist die der Heatmap (BRAND.md 3.3, `viewer/heatmap.ts`); ein Wert jenseits
// der Streckgrenze bekommt die Grenzfarbe, nie das obere Ende der Rampe.

import type { StressSummary } from "../engine/protocol.ts";
import { OVER_COLOR, RAMP, binOf, legendBins, type LegendBin, type Scale } from "../viewer/heatmap.ts";

export type StressQuantity = "von-mises" | "safety" | "displacement";

export const STRESS_QUANTITIES: readonly { id: StressQuantity; label: string; unit: string; hint: string }[] = [
  { id: "von-mises", label: "Spannung", unit: "MPa", hint: "Vergleichsspannung nach von Mises, rot ab der Streckgrenze" },
  { id: "safety", label: "Sicherheit", unit: "", hint: "Sicherheitsfaktor gegen Fließen und Schichtbruch, rot unter 1" },
  { id: "displacement", label: "Verformung", unit: "mm", hint: "Verschiebung der Zelle gegenüber dem unbelasteten Teil" },
];

/** Sicherheitsfaktoren ab hier gelten als „sicher“ und füllen das untere Ende der Rampe. */
export const SAFETY_TOP = 5;

export interface StressScale {
  quantity: StressQuantity;
  /** Rampe in den Einheiten der Größe; bei der Sicherheit ist 1 (Grenze) das helle und `SAFETY_TOP` das dunkle Ende. */
  scale: Scale;
}

export function stressScale(quantity: StressQuantity, summary: StressSummary, yieldStrength: number): StressScale {
  if (quantity === "von-mises") {
    return { quantity, scale: { min: 0, max: Math.max(summary.max_von_mises.value, 1e-9), limit: yieldStrength } };
  }
  if (quantity === "safety") return { quantity, scale: { min: 1, max: SAFETY_TOP, limit: null } };
  return { quantity, scale: { min: 0, max: Math.max(summary.max_displacement.value, 1e-9), limit: null } };
}

/** Der Farbindex 0..10 für einen Wert, oder -1 für die Grenzfarbe. */
export function binFor(stress: StressScale, value: number): number {
  if (stress.quantity === "safety") {
    if (value < 1) return -1;
    // Niedrige Sicherheit hell (wie hohe Spannung), hohe Sicherheit dunkel.
    return RAMP.length - 1 - binOf(Math.min(value, SAFETY_TOP), stress.scale);
  }
  return binOf(value, stress.scale);
}

export function colorFor(stress: StressScale, value: number): string {
  const bin = binFor(stress, value);
  return bin === -1 ? OVER_COLOR : RAMP[bin]!;
}

/** Die Legende von klein nach groß; bei der Sicherheit stehen die hellen Töne (kritisch) bei den kleinen Werten. */
export function stressLegend(stress: StressScale): LegendBin[] {
  const bins = legendBins(stress.scale);
  if (stress.quantity !== "safety") return bins;
  return bins.map((bin, index) => ({ ...bin, color: RAMP[RAMP.length - 1 - index]! }));
}

export function formatValue(quantity: StressQuantity, value: number): string {
  if (quantity === "safety") return value >= 100 ? value.toFixed(0) : value.toFixed(2).replace(".", ",");
  if (quantity === "displacement") return value.toFixed(value < 0.1 ? 4 : 3).replace(".", ",");
  return value.toFixed(value < 10 ? 2 : 1).replace(".", ",");
}
