import { useEffect, useRef } from "react";
import type { SliceResult } from "./engine/protocol.ts";

export interface PrintDialogProps {
  host: string;
  result: SliceResult;
  fileName: string;
  onConfirm: () => void;
  onCancel: () => void;
}

/** Starting a print moves a machine: it is always asked for, with the file and the printer named. */
export function PrintDialog({ host, result, fileName, onConfirm, onCancel }: PrintDialogProps) {
  const cancelRef = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    cancelRef.current?.focus();
  }, []);
  const minutes = Math.floor(result.time_s / 60);
  return (
    <div className="fixed inset-0 z-10 flex items-center justify-center bg-surface-chrome/80">
      <div
        role="alertdialog"
        aria-modal="true"
        aria-labelledby="print-title"
        onKeyDown={(event) => event.key === "Escape" && onCancel()}
        className="w-[480px] max-w-full border border-edge-strong bg-surface-panel font-ui text-ink"
      >
        <h2 id="print-title" className="m-0 border-b border-edge px-4 py-3 text-ks-section font-semibold text-ink-strong">
          Start a print?
        </h2>
        <dl className="m-0 grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 px-4 py-3 font-data text-ks-table">
          <dt className="text-ink-muted">Printer</dt>
          <dd className="m-0 text-ink-strong">{host}</dd>
          <dt className="text-ink-muted">File</dt>
          <dd className="m-0 text-ink-strong">{fileName}</dd>
          <dt className="text-ink-muted">Time</dt>
          <dd className="m-0 text-ink-strong">
            {Math.floor(minutes / 60)} h {minutes % 60} min
          </dd>
          <dt className="text-ink-muted">Filament</dt>
          <dd className="m-0 text-ink-strong">
            {(result.filament_mm / 1000).toFixed(2)} m {"·"} {result.filament_g.toFixed(1)} g
          </dd>
        </dl>
        <p className="m-0 px-4 pb-3 text-ks-table text-ink-muted">
          The printer starts at once. Check that the bed is clear and the filament is loaded.
        </p>
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
            className="h-target rounded border border-signal-caution bg-signal-caution-surface px-3 text-ks-ui font-semibold text-signal-caution hover:bg-surface-hover"
          >
            Upload and print
          </button>
        </div>
      </div>
    </div>
  );
}
