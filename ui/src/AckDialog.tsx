import { useEffect, useRef } from "react";
import type { Violation } from "./engine/protocol.ts";
import { formatNumber } from "./cockpit/model.ts";

const decimalsOf = (value: number): number => (Number.isInteger(value) ? 0 : 1);

export interface AckDialogProps {
  violations: readonly Violation[];
  onConfirm: () => void;
  onCancel: () => void;
}

/** Export with open violations needs an explicit acknowledgement that lists every one (BRAND.md 5.1). */
export function AckDialog({ violations, onConfirm, onCancel }: AckDialogProps) {
  const cancelRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    cancelRef.current?.focus();
  }, []);

  return (
    <div className="fixed inset-0 z-10 flex items-center justify-center bg-surface-chrome/80">
      <div
        role="alertdialog"
        aria-modal="true"
        aria-labelledby="ack-title"
        onKeyDown={(event) => event.key === "Escape" && onCancel()}
        className="w-[560px] max-w-full border border-edge-strong bg-surface-panel font-ui text-ink"
      >
        <h2 id="ack-title" className="m-0 border-b border-edge px-4 py-3 text-ks-section font-semibold text-signal-limit">
          {violations.length} limit {violations.length === 1 ? "violation" : "violations"} open
        </h2>
        <p className="m-0 px-4 pt-3 text-ks-table text-ink-secondary">
          Klipper does not clamp these values. The G-code will ask the machine for more than its limits.
        </p>
        <table className="m-4 w-[calc(100%-2rem)] border-collapse font-data text-ks-table">
          <thead>
            <tr className="text-left text-ink-muted">
              <th className="pb-1 font-normal">Parameter</th>
              <th className="pb-1 text-right font-normal">Value</th>
              <th className="pb-1 text-right font-normal">Limit</th>
              <th className="pb-1 pl-3 font-normal">Source</th>
            </tr>
          </thead>
          <tbody>
            {violations.map((violation) => (
              <tr key={`${violation.key}:${violation.kind}`} className="border-t border-edge-subtle">
                <td className="py-1 text-ink" title={violation.key}>
                  {violation.label}
                </td>
                <td className="py-1 text-right text-signal-limit">
                  {formatNumber(violation.value, decimalsOf(violation.value))} {violation.unit}
                </td>
                <td className="py-1 text-right text-ink-secondary">{formatNumber(violation.limit, decimalsOf(violation.limit))}</td>
                <td className="py-1 pl-3 text-ink-muted">{violation.limit_key}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <div className="flex justify-end gap-2 border-t border-edge px-4 py-3">
          <button
            ref={cancelRef}
            type="button"
            onClick={onCancel}
            className="h-target rounded border border-edge-strong bg-surface-raised px-3 text-ks-ui text-ink hover:bg-surface-hover"
          >
            Cancel
          </button>
          <button
            type="button"
            onClick={onConfirm}
            className="h-target rounded border border-signal-limit bg-signal-limit-surface px-3 text-ks-ui font-semibold text-signal-limit hover:bg-surface-hover"
          >
            Slice anyway
          </button>
        </div>
      </div>
    </div>
  );
}
