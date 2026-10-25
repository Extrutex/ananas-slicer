/**
 * KinematicsSidebar — the primary "Kinematics" parameter table of the
 * ANANAS SLICER cockpit sidebar.
 *
 * Design reference implementation (React + Tailwind, no UI libraries). The
 * production app is wxWidgets; this file pins down the behaviour and the
 * visual states the native OG_CustomCtrl/SpinInput rows have to reproduce.
 * Tailwind theme: docs/design/tailwind.tokens.js + docs/design/tokens.css.
 *
 * Why a limit column exists at all: Klipper does NOT clamp M204 /
 * SET_VELOCITY_LIMIT to printer.cfg's max_accel or square_corner_velocity
 * (behaviour since the 2021-04-30 change). Whatever the slicer emits is what
 * the machine executes, so the slicer is the last place an envelope violation
 * can be caught. The limit is shown per row and a violation is never hidden
 * by collapsing its group.
 */
import { useCallback, useEffect, useId, useMemo, useRef, useState } from "react";
import type { KeyboardEvent, PointerEvent } from "react";

/* -------------------------------------------------------------------------- */
/* Model                                                                      */
/* -------------------------------------------------------------------------- */

/**
 * Where a limit comes from, which decides how a violation is worded:
 * - machine:   printer.cfg envelope Klipper does not enforce at runtime
 * - firmware:  a bound Klipper itself enforces (the command errors out)
 * - profile:   a ANANAS SLICER hardware profile value (e.g. hotend melt rate)
 * - reference: the printer.cfg value, shown for comparison only — a slicer
 *              override is legitimate and not a violation
 */
export type LimitKind = "machine" | "firmware" | "profile" | "reference";

export interface ParamLimit {
  value: number;
  /** Dotted source, e.g. "printer.max_accel". Shown verbatim. */
  source: string;
  kind: LimitKind;
}

export interface KinematicsParam {
  id: string;
  label: string;
  /** Slicer config key, shown as tooltip so engineers can grep for it. */
  configKey: string;
  unit: string;
  value: number;
  step: number;
  decimals: number;
  /** Input bounds. Values outside are rejected as invalid, never clamped. */
  min: number;
  max: number;
  limit: ParamLimit;
}

export interface KinematicsGroup {
  id: string;
  title: string;
  params: KinematicsParam[];
}

export type LimitState = "nominal" | "caution" | "exceeded" | "reference";

/** At or above this share of a limit the row switches to caution. */
export const CAUTION_RATIO = 0.9;

export function evaluateLimit(value: number, limit: ParamLimit): LimitState {
  if (limit.kind === "reference") return "reference";
  if (value > limit.value) return "exceeded";
  if (value >= limit.value * CAUTION_RATIO) return "caution";
  return "nominal";
}

/* -------------------------------------------------------------------------- */
/* Number formatting — tabular, ISO 80000 digit grouping (narrow NBSP)        */
/* -------------------------------------------------------------------------- */

const GROUP_SEPARATOR = " ";

export function roundTo(value: number, decimals: number): number {
  const factor = 10 ** decimals;
  return Math.round(value * factor) / factor;
}

export function formatNumber(value: number, decimals: number): string {
  const fixed = Math.abs(value).toFixed(decimals);
  const [integer, fraction] = fixed.split(".");
  const grouped = integer.length > 3 ? integer.replace(/\B(?=(\d{3})+(?!\d))/g, GROUP_SEPARATOR) : integer;
  const sign = value < 0 ? "−" : "";
  return fraction !== undefined ? `${sign}${grouped}.${fraction}` : `${sign}${grouped}`;
}

const NUMBER_PATTERN = /^-?(\d+\.?\d*|\.\d+)$/;

/** Accepts "12 000", "12000", "0,035" (German keyboard) — rejects "1e4". */
export function parseNumber(text: string): number | null {
  const normalised = text.replace(/[\s ]/g, "").replace(",", ".").replace("−", "-");
  if (!NUMBER_PATTERN.test(normalised)) return null;
  const value = Number(normalised);
  return Number.isFinite(value) ? value : null;
}

type DraftCheck = { ok: true; value: number } | { ok: false; message: string };

function checkDraft(text: string, param: KinematicsParam): DraftCheck {
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

/** Keyboard/pointer step multiplier: Shift ×10, Alt ×0.1, otherwise ×1. */
function stepMultiplier(event: { shiftKey: boolean; altKey: boolean }): number {
  if (event.shiftKey) return 10;
  if (event.altKey) return 0.1;
  return 1;
}

function limitMessage(param: KinematicsParam, state: LimitState): string | null {
  const { limit, unit, decimals, value } = param;
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

/* -------------------------------------------------------------------------- */
/* Press-and-hold repeat for the micro-steppers                               */
/* -------------------------------------------------------------------------- */

const REPEAT_DELAY_MS = 400;
const REPEAT_INTERVAL_MS = 60;

function useAutoRepeat(action: (multiplier: number) => void) {
  const actionRef = useRef(action);
  const multiplierRef = useRef(1);
  const delayRef = useRef<number | null>(null);
  const intervalRef = useRef<number | null>(null);

  useEffect(() => {
    actionRef.current = action;
  }, [action]);

  const stop = useCallback(() => {
    if (delayRef.current !== null) window.clearTimeout(delayRef.current);
    if (intervalRef.current !== null) window.clearInterval(intervalRef.current);
    delayRef.current = null;
    intervalRef.current = null;
  }, []);

  const start = useCallback(
    (event: PointerEvent<HTMLButtonElement>) => {
      if (event.button !== 0) return;
      event.preventDefault();
      stop();
      multiplierRef.current = stepMultiplier(event);
      actionRef.current(multiplierRef.current);
      delayRef.current = window.setTimeout(() => {
        intervalRef.current = window.setInterval(() => actionRef.current(multiplierRef.current), REPEAT_INTERVAL_MS);
      }, REPEAT_DELAY_MS);
    },
    [stop],
  );

  useEffect(() => stop, [stop]);

  return { start, stop };
}

/* -------------------------------------------------------------------------- */
/* Row                                                                        */
/* -------------------------------------------------------------------------- */

const ROW_GRID = "grid grid-cols-[minmax(0,1fr)_120px_50px_84px] items-center gap-x-2";

interface StepperButtonProps {
  direction: 1 | -1;
  param: KinematicsParam;
  disabled: boolean;
  onStep: (direction: 1 | -1, multiplier: number) => void;
}

function StepperButton({ direction, param, disabled, onStep }: StepperButtonProps) {
  const action = useCallback((multiplier: number) => onStep(direction, multiplier), [direction, onStep]);
  const { start, stop } = useAutoRepeat(action);

  // A button that turns disabled mid-hold (bound reached) stops receiving
  // pointer events, so pointerup would never end the repeat.
  useEffect(() => {
    if (disabled) stop();
  }, [disabled, stop]);
  const verb = direction === 1 ? "Increase" : "Decrease";
  return (
    <button
      type="button"
      tabIndex={-1}
      disabled={disabled}
      aria-label={`${verb} ${param.label} by ${formatNumber(param.step, param.decimals)} ${param.unit}`}
      title={`${verb} · Shift ×10 · Alt ×0.1`}
      onPointerDown={start}
      onPointerUp={stop}
      onPointerLeave={stop}
      onPointerCancel={stop}
      className="flex h-target w-target items-center justify-center rounded border border-edge bg-surface-raised font-data text-ks-ui leading-none text-ink-secondary transition-colors duration-fast hover:border-edge-strong hover:bg-surface-hover hover:text-ink-strong active:bg-carbon-700 disabled:cursor-not-allowed disabled:text-ink-disabled"
    >
      {direction === 1 ? "+" : "−"}
    </button>
  );
}

interface ParamRowProps {
  param: KinematicsParam;
  onValueChange: (paramId: string, value: number) => void;
}

function ParamRow({ param, onValueChange }: ParamRowProps) {
  const inputId = useId();
  const messageId = useId();
  const [draft, setDraft] = useState<string | null>(null);

  const draftCheck = draft === null ? null : checkDraft(draft, param);
  const invalidMessage = draftCheck && !draftCheck.ok ? draftCheck.message : null;
  const state = evaluateLimit(param.value, param.limit);
  const message = invalidMessage ?? limitMessage(param, state);
  const rawValue = param.value.toFixed(param.decimals);
  const isEditing = draft !== null;

  const commit = useCallback(
    (value: number) => {
      const next = roundTo(value, param.decimals);
      if (next !== param.value) onValueChange(param.id, next);
      return next;
    },
    [onValueChange, param.decimals, param.id, param.value],
  );

  const stepBy = useCallback(
    (direction: 1 | -1, multiplier: number) => {
      // Step from the draft if it is a valid number, otherwise from the
      // committed value. Out-of-range results stop at the input bound; the
      // LIMIT is never used as a clamp — exceeding it is a visible state.
      const base = draftCheck && draftCheck.ok ? draftCheck.value : param.value;
      const target = Math.min(param.max, Math.max(param.min, base + direction * param.step * multiplier));
      const next = commit(target);
      if (isEditing) setDraft(next.toFixed(param.decimals));
    },
    [commit, draftCheck, isEditing, param.decimals, param.max, param.min, param.step, param.value],
  );

  const handleKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    switch (event.key) {
      case "ArrowUp":
        event.preventDefault();
        stepBy(1, stepMultiplier(event));
        break;
      case "ArrowDown":
        event.preventDefault();
        stepBy(-1, stepMultiplier(event));
        break;
      case "PageUp":
        event.preventDefault();
        stepBy(1, 10);
        break;
      case "PageDown":
        event.preventDefault();
        stepBy(-1, 10);
        break;
      case "Enter":
        if (draftCheck && draftCheck.ok) {
          const next = commit(draftCheck.value);
          setDraft(next.toFixed(param.decimals));
        }
        break;
      case "Escape":
        event.preventDefault();
        setDraft(rawValue);
        break;
      default:
        break;
    }
  };

  const handleBlur = () => {
    if (draftCheck && draftCheck.ok) {
      commit(draftCheck.value);
      setDraft(null);
    }
    // An invalid draft stays visible with its error: silently reverting
    // would hide that the user's input was not applied.
  };

  const barClass =
    invalidMessage || state === "exceeded"
      ? "bg-signal-limit"
      : state === "caution"
        ? "bg-signal-caution"
        : "bg-transparent";

  const rowSurface = state === "exceeded" && !invalidMessage ? "bg-signal-limit-surface" : "hover:bg-surface-hover";

  const valueInk = invalidMessage || state === "exceeded" ? "text-signal-limit" : "text-ink-strong";

  const fieldEdge = invalidMessage
    ? "border-signal-limit"
    : state === "exceeded"
      ? "border-signal-limit"
      : "border-edge-field";

  const limitInk =
    state === "exceeded"
      ? "font-semibold text-signal-limit"
      : state === "caution"
        ? "text-signal-caution"
        : "text-ink-muted";

  const limitPrefix = param.limit.kind === "reference" ? (param.value === param.limit.value ? "cfg" : "≠ cfg") : "≤";

  const messageTag = invalidMessage ? "INVALID" : state === "exceeded" ? "LIMIT" : state === "caution" ? "NEAR" : null;
  const messageInk = invalidMessage || state === "exceeded" ? "text-signal-limit" : "text-signal-caution";
  const messageTagClass =
    invalidMessage || state === "exceeded"
      ? "bg-signal-limit text-ink-on-signal"
      : "border border-signal-caution text-signal-caution";

  return (
    <div className={`relative border-b border-edge-subtle transition-colors duration-fast ${rowSurface}`}>
      <span aria-hidden="true" className={`absolute inset-y-0 left-0 w-[3px] ${barClass}`} />
      <div className={`${ROW_GRID} min-h-[28px] pl-4 pr-3`}>
        <label
          htmlFor={inputId}
          title={param.configKey}
          className="truncate text-ks-table text-ink-secondary"
        >
          {param.label}
        </label>

        <div
          className={`flex h-target items-center rounded border bg-surface-field focus-within:outline focus-within:outline-2 focus-within:outline-offset-1 focus-within:outline-edge-focus ${fieldEdge}`}
        >
          <input
            id={inputId}
            type="text"
            inputMode="decimal"
            autoComplete="off"
            spellCheck={false}
            aria-invalid={invalidMessage !== null || state === "exceeded"}
            aria-describedby={message ? messageId : undefined}
            value={draft ?? formatNumber(param.value, param.decimals)}
            onFocus={(event) => {
              setDraft(rawValue);
              event.currentTarget.select();
            }}
            onChange={(event) => setDraft(event.target.value)}
            onKeyDown={handleKeyDown}
            onBlur={handleBlur}
            className={`h-full min-w-0 flex-1 bg-transparent pl-2 text-right font-data text-ks-table tabular-nums outline-none ${valueInk}`}
          />
          <span className="w-[6ch] shrink-0 pl-1.5 font-data text-ks-table text-ink-muted" aria-hidden="true">
            {param.unit}
          </span>
        </div>

        <div className="flex gap-0.5">
          <StepperButton direction={-1} param={param} disabled={param.value <= param.min} onStep={stepBy} />
          <StepperButton direction={1} param={param} disabled={param.value >= param.max} onStep={stepBy} />
        </div>

        <span
          title={`${param.limit.source} = ${formatNumber(param.limit.value, param.decimals)} ${param.unit}`}
          className={`truncate text-right font-data text-ks-table tabular-nums ${limitInk}`}
        >
          <span className="text-ink-muted">{limitPrefix} </span>
          {formatNumber(param.limit.value, param.decimals)}
        </span>
      </div>

      {message && messageTag ? (
        <p id={messageId} className={`flex items-start gap-2 pb-2 pl-4 pr-3 text-ks-table ${messageInk}`}>
          <span className={`mt-px shrink-0 rounded px-1 font-data text-ks-micro font-semibold ${messageTagClass}`}>
            {messageTag}
          </span>
          <span>{message}</span>
        </p>
      ) : null}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Group                                                                      */
/* -------------------------------------------------------------------------- */

function countStates(params: KinematicsParam[]) {
  let exceeded = 0;
  let caution = 0;
  for (const param of params) {
    const state = evaluateLimit(param.value, param.limit);
    if (state === "exceeded") exceeded += 1;
    else if (state === "caution") caution += 1;
  }
  return { exceeded, caution };
}

function sharedSource(params: KinematicsParam[]): string | null {
  const first = params[0]?.limit.source;
  return first && params.every((param) => param.limit.source === first) ? first : null;
}

interface GroupSectionProps {
  group: KinematicsGroup;
  expanded: boolean;
  onToggle: (groupId: string) => void;
  onValueChange: (paramId: string, value: number) => void;
}

function GroupSection({ group, expanded, onToggle, onValueChange }: GroupSectionProps) {
  const regionId = useId();
  const { exceeded, caution } = countStates(group.params);
  const source = sharedSource(group.params);

  return (
    <section>
      <h3 className="m-0">
        <button
          type="button"
          aria-expanded={expanded}
          aria-controls={regionId}
          onClick={() => onToggle(group.id)}
          className="flex h-row-header w-full items-center gap-2 border-b border-edge bg-surface-raised px-3 text-left transition-colors duration-fast hover:bg-surface-hover"
        >
          <span
            aria-hidden="true"
            className={`inline-block w-3 text-ks-micro text-ink-muted transition-transform duration-fast ${expanded ? "rotate-90" : ""}`}
          >
            {"▸"}
          </span>
          <span className="text-ks-ui font-semibold text-ink">{group.title}</span>
          <span className="font-data text-ks-table text-ink-muted">{group.params.length}</span>
          <span className="ml-auto flex min-w-0 items-center gap-1.5">
            {source ? (
              <span className="truncate font-data text-ks-table text-ink-muted" title="Limit source">
                {source}
              </span>
            ) : null}
            {exceeded > 0 ? (
              <span className="shrink-0 rounded bg-signal-limit px-1.5 font-data text-ks-micro font-semibold text-ink-on-signal">
                {exceeded} OVER
              </span>
            ) : null}
            {caution > 0 ? (
              <span className="shrink-0 rounded border border-signal-caution px-1.5 font-data text-ks-micro font-semibold text-signal-caution">
                {caution} NEAR
              </span>
            ) : null}
          </span>
        </button>
      </h3>
      <div id={regionId} role="group" aria-label={group.title} hidden={!expanded}>
        {group.params.map((param) => (
          <ParamRow key={param.id} param={param} onValueChange={onValueChange} />
        ))}
      </div>
    </section>
  );
}

/* -------------------------------------------------------------------------- */
/* Sidebar                                                                    */
/* -------------------------------------------------------------------------- */

export interface KinematicsSidebarProps {
  groups: KinematicsGroup[];
  onValueChange: (paramId: string, value: number) => void;
  /** e.g. "Voron 2.4 350" */
  machineLabel: string;
  /** e.g. "printer.cfg · read 14:02:11" */
  configSource: string;
  /** Set when the values are not read from a live machine. */
  exampleData?: boolean;
  initiallyCollapsed?: readonly string[];
}

export function KinematicsSidebar({
  groups,
  onValueChange,
  machineLabel,
  configSource,
  exampleData = false,
  initiallyCollapsed = [],
}: KinematicsSidebarProps) {
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set(initiallyCollapsed));

  const toggle = useCallback((groupId: string) => {
    setCollapsed((previous) => {
      const next = new Set(previous);
      if (next.has(groupId)) next.delete(groupId);
      else next.add(groupId);
      return next;
    });
  }, []);

  const totals = useMemo(() => countStates(groups.flatMap((group) => group.params)), [groups]);

  return (
    <aside
      aria-label="Kinematics parameters"
      className="flex h-full w-sidebar shrink-0 flex-col border-r border-edge bg-surface-panel font-ui text-ink"
    >
      <header className="border-b border-edge px-3 py-2.5">
        <div className="flex items-baseline justify-between gap-2">
          <h2 className="m-0 text-ks-section font-semibold text-ink-strong">Kinematics</h2>
          {exampleData ? (
            <span className="rounded border border-edge-strong px-1.5 font-data text-ks-micro text-ink-muted">
              EXAMPLE DATA
            </span>
          ) : null}
        </div>
        <p className="m-0 mt-0.5 truncate font-data text-ks-table text-ink-muted">
          {machineLabel} · {configSource}
        </p>
        <p
          role="status"
          aria-live="polite"
          className={`m-0 mt-2 text-ks-table ${totals.exceeded > 0 ? "text-signal-limit" : totals.caution > 0 ? "text-signal-caution" : "text-ink-muted"}`}
        >
          {totals.exceeded > 0
            ? `${totals.exceeded} parameter${totals.exceeded === 1 ? "" : "s"} exceed${totals.exceeded === 1 ? "s" : ""} a machine limit. Export requires explicit acknowledgement.`
            : totals.caution > 0
              ? `All values within limits · ${totals.caution} within 10 % of a limit.`
              : "All values within limits."}
        </p>
      </header>

      <div
        aria-hidden="true"
        className={`${ROW_GRID} h-6 border-b border-edge bg-surface-panel pl-4 pr-3 font-ui text-ks-micro uppercase text-ink-muted`}
      >
        <span>Parameter</span>
        <span className="text-right">Value · unit</span>
        <span className="text-center">Step</span>
        <span className="text-right">Limit</span>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto">
        {groups.map((group) => (
          <GroupSection
            key={group.id}
            group={group}
            expanded={!collapsed.has(group.id)}
            onToggle={toggle}
            onValueChange={onValueChange}
          />
        ))}
      </div>

      <footer className="border-t border-edge px-3 py-2 font-data text-ks-micro text-ink-muted">
        <kbd>↑</kbd>/<kbd>↓</kbd> step · <kbd>Shift</kbd> ×10 · <kbd>Alt</kbd> ×0.1 · <kbd>Enter</kbd> apply ·{" "}
        <kbd>Esc</kbd> revert
      </footer>
    </aside>
  );
}

/* -------------------------------------------------------------------------- */
/* Example data + stateful demo                                               */
/* -------------------------------------------------------------------------- */

const MAX_ACCEL: ParamLimit = { value: 10000, source: "printer.max_accel", kind: "machine" };

function accel(id: string, label: string, configKey: string, value: number): KinematicsParam {
  return { id, label, configKey, unit: "mm/s²", value, step: 100, decimals: 0, min: 0, max: 50000, limit: MAX_ACCEL };
}

/** Example values only — not read from any real machine. */
export const DEMO_GROUPS: KinematicsGroup[] = [
  {
    id: "acceleration",
    title: "Acceleration",
    params: [
      accel("accel-outer-wall", "Outer wall", "outer_wall_acceleration", 3000),
      accel("accel-inner-wall", "Inner wall", "inner_wall_acceleration", 6000),
      accel("accel-infill", "Sparse infill", "sparse_infill_acceleration", 12000),
      accel("accel-top", "Top surface", "top_surface_acceleration", 2500),
      accel("accel-travel", "Travel", "travel_acceleration", 9500),
      accel("accel-first-layer", "First layer", "initial_layer_acceleration", 1500),
    ],
  },
  {
    id: "cornering",
    title: "Cornering",
    params: [
      {
        id: "scv",
        label: "Square corner velocity",
        configKey: "square_corner_velocity",
        unit: "mm/s",
        value: 5,
        step: 0.5,
        decimals: 1,
        min: 0,
        max: 50,
        limit: { value: 8, source: "printer.square_corner_velocity", kind: "machine" },
      },
    ],
  },
  {
    id: "flow",
    title: "Volumetric flow",
    params: [
      {
        id: "max-flow",
        label: "Max volumetric flow",
        configKey: "filament_max_volumetric_speed",
        unit: "mm³/s",
        value: 18,
        step: 0.5,
        decimals: 1,
        min: 0,
        max: 100,
        limit: { value: 24, source: "hotend.max_volumetric_flow", kind: "profile" },
      },
    ],
  },
  {
    id: "pressure-advance",
    title: "Pressure advance",
    params: [
      {
        id: "pa",
        label: "Pressure advance",
        configKey: "pressure_advance",
        unit: "s",
        value: 0.035,
        step: 0.001,
        decimals: 3,
        min: 0,
        max: 1,
        limit: { value: 0.04, source: "extruder.pressure_advance", kind: "reference" },
      },
      {
        id: "pa-smooth",
        label: "Smooth time",
        configKey: "pressure_advance_smooth_time",
        unit: "s",
        value: 0.04,
        step: 0.005,
        decimals: 3,
        min: 0,
        max: 1,
        limit: { value: 0.2, source: "extruder.pressure_advance_smooth_time", kind: "firmware" },
      },
    ],
  },
];

export function KinematicsSidebarDemo() {
  const [groups, setGroups] = useState<KinematicsGroup[]>(DEMO_GROUPS);

  const handleValueChange = useCallback((paramId: string, value: number) => {
    setGroups((previous) =>
      previous.map((group) => ({
        ...group,
        params: group.params.map((param) => (param.id === paramId ? { ...param, value } : param)),
      })),
    );
  }, []);

  return (
    <KinematicsSidebar
      groups={groups}
      onValueChange={handleValueChange}
      machineLabel="Voron 2.4 350"
      configSource="printer.cfg"
      exampleData
    />
  );
}

export default KinematicsSidebarDemo;
