// Pure model of a cockpit parameter row: limit states, number formatting and parsing, step rules.
// Behaviour pinned down by docs/design/BRAND.md 5.2; ported from docs/design/preview/KinematicsSidebar.tsx.

/**
 * Where a limit comes from, which decides how a violation is worded:
 * - machine:   printer.cfg envelope Klipper does not enforce at runtime
 * - firmware:  a bound Klipper itself enforces (the command errors out)
 * - profile:   a hardware profile value (e.g. hotend melt rate)
 * - reference: the printer.cfg value, shown for comparison only; a slicer override is legitimate
 */
export type LimitKind = "machine" | "firmware" | "profile" | "reference";

export interface ParamLimit {
  value: number;
  /** Dotted source, e.g. "printer.max_accel". Shown verbatim. */
  source: string;
  kind: LimitKind;
}

export interface CockpitParam {
  /** The slicer config key; also the row's identity. Shown as the tooltip so engineers can grep for it. */
  id: string;
  label: string;
  unit: string;
  value: number;
  step: number;
  decimals: number;
  /** Input bounds. Values outside are rejected as invalid, never clamped. */
  min: number;
  max: number;
  /** What the row is held against; null when there is nothing to compare with (no printer.cfg imported). */
  limit: ParamLimit | null;
  /** A derived value cannot be edited. */
  readOnly?: boolean;
}

export interface CockpitGroup {
  id: string;
  title: string;
  params: CockpitParam[];
}

export type LimitState = "nominal" | "caution" | "exceeded" | "reference";

/** At or above this share of a limit the row switches to caution. */
export const CAUTION_RATIO = 0.9;

export function evaluateLimit(value: number, limit: ParamLimit | null): LimitState {
  if (limit === null) return "nominal";
  if (limit.kind === "reference") return "reference";
  if (value > limit.value) return "exceeded";
  if (value >= limit.value * CAUTION_RATIO) return "caution";
  return "nominal";
}

// Number formatting: tabular, ISO 80000 digit grouping (narrow no-break space).
const GROUP_SEPARATOR = " ";

export function roundTo(value: number, decimals: number): number {
  const factor = 10 ** decimals;
  return Math.round(value * factor) / factor;
}

export function formatNumber(value: number, decimals: number): string {
  const fixed = Math.abs(value).toFixed(decimals);
  const [integer = "", fraction] = fixed.split(".");
  const grouped = integer.length > 3 ? integer.replace(/\B(?=(\d{3})+(?!\d))/g, GROUP_SEPARATOR) : integer;
  const sign = value < 0 ? "−" : "";
  return fraction !== undefined ? `${sign}${grouped}.${fraction}` : `${sign}${grouped}`;
}

const NUMBER_PATTERN = /^-?(\d+\.?\d*|\.\d+)$/;

/** Accepts "12 000", "12000", "0,035" (German keyboard); rejects "1e4". */
export function parseNumber(text: string): number | null {
  const normalised = text.replace(/[\s  ]/g, "").replace(",", ".").replace("−", "-");
  if (!NUMBER_PATTERN.test(normalised)) return null;
  const value = Number(normalised);
  return Number.isFinite(value) ? value : null;
}

export type DraftCheck = { ok: true; value: number } | { ok: false; message: string };

export function checkDraft(text: string, param: CockpitParam): DraftCheck {
  const value = parseNumber(text);
  if (value === null) return { ok: false, message: "Not a number." };
  if (value < param.min || value > param.max) {
    return {
      ok: false,
      message: `Outside input range ${formatNumber(param.min, param.decimals)} – ${formatNumber(param.max, param.decimals)} ${param.unit}.`,
    };
  }
  return { ok: true, value: roundTo(value, param.decimals) };
}

/** Keyboard/pointer step multiplier: Shift x10, Alt x0.1, otherwise x1. */
export function stepMultiplier(event: { shiftKey: boolean; altKey: boolean }): number {
  if (event.shiftKey) return 10;
  if (event.altKey) return 0.1;
  return 1;
}

/** The value one step from `base`, held at the input bounds. The limit is never a clamp. */
export function steppedValue(param: CockpitParam, base: number, direction: 1 | -1, multiplier: number): number {
  const target = Math.min(param.max, Math.max(param.min, base + direction * param.step * multiplier));
  return roundTo(target, param.decimals);
}

export function limitMessage(param: CockpitParam, state: LimitState): string | null {
  const { limit, unit, decimals, value } = param;
  if (limit === null) return null;
  const limitText = `${formatNumber(limit.value, decimals)} ${unit}`;
  if (state === "exceeded") {
    const over = formatNumber(roundTo(value - limit.value, decimals), decimals);
    switch (limit.kind) {
      case "machine":
        return `Exceeds ${limit.source} (${limitText}) by ${over}. Klipper does not clamp this value.`;
      case "firmware":
        return `Above Klipper's maximum for ${limit.source} (${limitText}). The command is rejected; a running print stops with an error.`;
      case "profile":
        return `Exceeds ${limit.source} (${limitText}) by ${over}. Klipper does not enforce volumetric flow.`;
      default:
        return null;
    }
  }
  if (state === "caution") {
    const share = Math.round((value / limit.value) * 100);
    return `${share} % of ${limit.source} (${limitText}).`;
  }
  return null;
}

export function countStates(params: readonly CockpitParam[]): { exceeded: number; caution: number } {
  let exceeded = 0;
  let caution = 0;
  for (const param of params) {
    const state = evaluateLimit(param.value, param.limit);
    if (state === "exceeded") exceeded += 1;
    else if (state === "caution") caution += 1;
  }
  return { exceeded, caution };
}

export function sharedSource(params: readonly CockpitParam[]): string | null {
  const first = params[0]?.limit?.source;
  return first && params.every((param) => param.limit?.source === first) ? first : null;
}
