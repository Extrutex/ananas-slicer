import { useEffect, useState } from "react";

const FIELD =
  "h-target w-full min-w-0 rounded border border-edge-field bg-surface-field px-2 font-data text-ks-table text-ink-strong aria-[invalid=true]:border-signal-limit";

/** Deutsche und englische Schreibweise: "2,5" und "2.5". Leer und Text sind keine Zahl. */
export function parseNumber(text: string): number | null {
  const trimmed = text.trim().replace(",", ".");
  if (trimmed === "") return null;
  const value = Number(trimmed);
  return Number.isFinite(value) ? value : null;
}

export function formatNumber(value: number): string {
  return String(value).replace(".", ",");
}

export interface NumberFieldProps {
  label: string;
  value: number;
  onChange: (value: number) => void;
  unit?: string;
  /** Wird mit dem Text des Feldes verglichen, wenn der Wert von außen kommt. */
  testId?: string;
}

/**
 * Ein Zahlenfeld, das nie klemmt: was keine Zahl ist, bleibt sichtbar als ungültig markiert und ändert den Wert
 * nicht, bis es korrigiert wird. Wertebereiche prüft die Einrichtung (`checkSetup`), nicht das Feld.
 */
export function NumberField({ label, value, onChange, unit, testId }: NumberFieldProps) {
  const [text, setText] = useState(formatNumber(value));
  const [invalid, setInvalid] = useState(false);

  useEffect(() => {
    // Ein neuer Wert von außen (z. B. eine Voreinstellung) ersetzt den Text, ein Tippfehler nicht.
    setText((current) => (parseNumber(current) === value ? current : formatNumber(value)));
    setInvalid(false);
  }, [value]);

  return (
    <label className="flex min-w-0 flex-col gap-0.5 text-ks-micro text-ink-muted">
      <span className="truncate">
        {label}
        {unit ? ` (${unit})` : ""}
      </span>
      <input
        aria-label={label}
        aria-invalid={invalid}
        data-testid={testId}
        type="text"
        inputMode="decimal"
        spellCheck={false}
        value={text}
        onChange={(event) => {
          setText(event.target.value);
          const parsed = parseNumber(event.target.value);
          setInvalid(parsed === null);
          if (parsed !== null) onChange(parsed);
        }}
        className={FIELD}
      />
    </label>
  );
}
