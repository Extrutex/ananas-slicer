import { useCallback, useEffect, useId, useRef, useState } from "react";
import type { KeyboardEvent, PointerEvent } from "react";
import {
  checkDraft,
  evaluateLimit,
  formatNumber,
  limitMessage,
  stepMultiplier,
  steppedValue,
  type CockpitParam,
} from "./model.ts";

// Press-and-hold repeat for the micro-steppers (BRAND.md 5.2: after 400 ms at 60 ms intervals).
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

export const ROW_GRID = "grid grid-cols-[minmax(0,1fr)_120px_50px_84px] items-center gap-x-2";

interface StepperButtonProps {
  direction: 1 | -1;
  param: CockpitParam;
  disabled: boolean;
  onStep: (direction: 1 | -1, multiplier: number) => void;
}

function StepperButton({ direction, param, disabled, onStep }: StepperButtonProps) {
  const action = useCallback((multiplier: number) => onStep(direction, multiplier), [direction, onStep]);
  const { start, stop } = useAutoRepeat(action);

  // A button that turns disabled mid-hold (bound reached) stops receiving pointer events,
  // so pointerup would never end the repeat.
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

export interface ParamRowProps {
  param: CockpitParam;
  /** The engine's reason for refusing the last input of this row, if it did. */
  rejected?: string;
  onValueChange: (paramId: string, value: number) => void;
  /** The user reverted the row, so the engine's refusal no longer applies. */
  onDismiss: (paramId: string) => void;
}

export function ParamRow({ param, rejected, onValueChange, onDismiss }: ParamRowProps) {
  const inputId = useId();
  const messageId = useId();
  const inputRef = useRef<HTMLInputElement>(null);
  const [draft, setDraft] = useState<string | null>(null);

  const draftCheck = draft === null ? null : checkDraft(draft, param);
  const invalidMessage = (draftCheck && !draftCheck.ok ? draftCheck.message : null) ?? rejected ?? null;
  const state = evaluateLimit(param.value, param.limit);
  const message = invalidMessage ?? limitMessage(param, state);
  const rawValue = param.value.toFixed(param.decimals);
  const isEditing = draft !== null;

  // Focusing swaps the grouped display value for the raw one. Select after that render, or the new text is
  // not selected and typing would append to it instead of replacing it.
  useEffect(() => {
    if (isEditing) inputRef.current?.select();
  }, [isEditing]);

  const commit = useCallback(
    (value: number) => {
      if (value !== param.value) onValueChange(param.id, value);
      return value;
    },
    [onValueChange, param.id, param.value],
  );

  const stepBy = useCallback(
    (direction: 1 | -1, multiplier: number) => {
      // Step from the draft if it is a valid number, otherwise from the committed value. A result outside the
      // input bounds stops at the bound; the LIMIT is never a clamp, exceeding it is a visible state.
      const base = draftCheck && draftCheck.ok ? draftCheck.value : param.value;
      const next = commit(steppedValue(param, base, direction, multiplier));
      if (isEditing) setDraft(next.toFixed(param.decimals));
    },
    [commit, draftCheck, isEditing, param],
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
        onDismiss(param.id);
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
    // An invalid draft stays visible with its error: silently reverting would hide that the input was not applied.
  };

  const barClass =
    invalidMessage || state === "exceeded" ? "bg-signal-limit" : state === "caution" ? "bg-signal-caution" : "bg-transparent";
  const rowSurface = state === "exceeded" && !invalidMessage ? "bg-signal-limit-surface" : "hover:bg-surface-hover";
  const valueInk = invalidMessage || state === "exceeded" ? "text-signal-limit" : "text-ink-strong";
  const fieldEdge = invalidMessage || state === "exceeded" ? "border-signal-limit" : "border-edge-field";
  const limitInk =
    state === "exceeded" ? "font-semibold text-signal-limit" : state === "caution" ? "text-signal-caution" : "text-ink-muted";
  const limitPrefix =
    param.limit?.kind === "reference" ? (param.value === param.limit.value ? "cfg" : "≠ cfg") : "≤";
  const messageTag = invalidMessage ? "INVALID" : state === "exceeded" ? "LIMIT" : state === "caution" ? "NEAR" : null;
  const messageInk = invalidMessage || state === "exceeded" ? "text-signal-limit" : "text-signal-caution";
  const messageTagClass =
    invalidMessage || state === "exceeded"
      ? "bg-signal-limit text-ink-on-signal"
      : "border border-signal-caution text-signal-caution";

  return (
    <div
      data-testid={`row-${param.id}`}
      data-state={invalidMessage ? "invalid" : state}
      className={`relative border-b border-edge-subtle transition-colors duration-fast ${rowSurface}`}
    >
      <span aria-hidden="true" className={`absolute inset-y-0 left-0 w-[3px] ${barClass}`} />
      <div className={`${ROW_GRID} min-h-[28px] pl-4 pr-3`}>
        <label htmlFor={inputId} title={param.id} className="truncate text-ks-table text-ink-secondary">
          {param.label}
        </label>

        <div
          className={`flex h-target items-center rounded border bg-surface-field focus-within:outline focus-within:outline-2 focus-within:outline-offset-1 focus-within:outline-edge-focus ${fieldEdge}`}
        >
          <input
            ref={inputRef}
            id={inputId}
            type="text"
            inputMode="decimal"
            autoComplete="off"
            spellCheck={false}
            readOnly={param.readOnly}
            aria-invalid={invalidMessage !== null || state === "exceeded"}
            aria-describedby={message ? messageId : undefined}
            value={draft ?? formatNumber(param.value, param.decimals)}
            onFocus={() => {
              if (!param.readOnly) setDraft(rawValue);
            }}
            onChange={(event) => setDraft(event.target.value)}
            onKeyDown={param.readOnly ? undefined : handleKeyDown}
            onBlur={param.readOnly ? undefined : handleBlur}
            className={`h-full min-w-0 flex-1 bg-transparent pl-2 text-right font-data text-ks-table tabular-nums outline-none ${valueInk}`}
          />
          <span className="w-[6ch] shrink-0 pl-1.5 font-data text-ks-table text-ink-muted" aria-hidden="true">
            {param.unit}
          </span>
        </div>

        <div className="flex gap-0.5">
          {param.readOnly ? null : (
            <>
              <StepperButton direction={-1} param={param} disabled={param.value <= param.min} onStep={stepBy} />
              <StepperButton direction={1} param={param} disabled={param.value >= param.max} onStep={stepBy} />
            </>
          )}
        </div>

        {param.limit ? (
          <span
            title={`${param.limit.source} = ${formatNumber(param.limit.value, param.decimals)} ${param.unit}`}
            className={`truncate text-right font-data text-ks-table tabular-nums ${limitInk}`}
          >
            <span className="text-ink-muted">{limitPrefix} </span>
            {formatNumber(param.limit.value, param.decimals)}
          </span>
        ) : (
          <span className="text-right font-data text-ks-table text-ink-muted" title="No printer.cfg imported">
            {"\u2014"}
          </span>
        )}
      </div>

      {message && messageTag ? (
        <p id={messageId} className={`flex items-start gap-2 pb-2 pl-4 pr-3 text-ks-table ${messageInk}`}>
          <span className={`mt-px shrink-0 rounded px-1 font-data text-ks-micro font-semibold ${messageTagClass}`}>{messageTag}</span>
          <span>{message}</span>
        </p>
      ) : null}
    </div>
  );
}
