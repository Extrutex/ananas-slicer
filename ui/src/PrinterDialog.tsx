import { useEffect, useRef, useState } from "react";
import type { CockpitSession, PrinterConnection } from "./state/session.ts";

const FIELD = "h-target w-full rounded border border-edge-field bg-surface-field px-2 font-data text-ks-table text-ink-strong";
const BUTTON = "h-target rounded border px-3 text-ks-ui";

export interface PrinterDialogProps {
  session: CockpitSession;
  printer: PrinterConnection;
  onClose: () => void;
}

/** Connects to a printer host (Moonraker or FLUXRAKER). The key is passed to the session and not kept here. */
export function PrinterDialog({ session, printer, onClose }: PrinterDialogProps) {
  const [host, setHost] = useState(printer.host);
  const [key, setKey] = useState("");
  const hostRef = useRef<HTMLInputElement>(null);
  const connecting = printer.status === "connecting";
  const connected = printer.status === "connected";

  useEffect(() => {
    hostRef.current?.focus();
  }, []);

  const connect = () => {
    if (host.trim() !== "") void session.connectPrinter(host.trim(), key);
  };

  return (
    <div className="fixed inset-0 z-10 flex items-center justify-center bg-surface-chrome/80">
      <div
        role="dialog"
        aria-modal="true"
        aria-labelledby="printer-title"
        onKeyDown={(event) => event.key === "Escape" && onClose()}
        className="w-[520px] max-w-full border border-edge-strong bg-surface-panel font-ui text-ink"
      >
        <h2 id="printer-title" className="m-0 border-b border-edge px-4 py-3 text-ks-section font-semibold text-ink-strong">
          Printer
        </h2>
        <div className="flex flex-col gap-3 p-4">
          <label className="flex flex-col gap-1 text-ks-table text-ink-muted">
            Address of Moonraker or FLUXRAKER
            <input
              ref={hostRef}
              aria-label="Printer address"
              type="text"
              spellCheck={false}
              placeholder="http://printer.local:7125"
              value={host}
              disabled={connected}
              onChange={(event) => setHost(event.target.value)}
              onKeyDown={(event) => event.key === "Enter" && !connected && connect()}
              className={FIELD}
            />
          </label>
          <label className="flex flex-col gap-1 text-ks-table text-ink-muted">
            API key (only if the printer asks for one)
            <input
              aria-label="API key"
              type="password"
              autoComplete="off"
              value={key}
              disabled={connected}
              onChange={(event) => setKey(event.target.value)}
              onKeyDown={(event) => event.key === "Enter" && !connected && connect()}
              className={FIELD}
            />
          </label>
          <p className="m-0 text-ks-table text-ink-muted">
            The key stays in memory while the app runs and is sent only to this address. Nothing is sent until you slice and choose to send.
          </p>

          {printer.status === "failed" ? (
            <p role="alert" className="m-0 text-ks-table text-signal-limit" data-testid="printer-error">
              {printer.error}
            </p>
          ) : null}
          {connected ? (
            <div className="border border-edge-subtle bg-surface-raised p-2 font-data text-ks-table" data-testid="printer-info">
              <div className="text-signal-nominal">Connected to {printer.host}</div>
              <div className="text-ink-secondary">
                Klipper {printer.klippyState || "unknown"} {"·"} Moonraker {printer.moonrakerVersion || "?"}
              </div>
              {printer.warnings.map((warning) => (
                <div key={warning} className="text-signal-caution">
                  {warning}
                </div>
              ))}
            </div>
          ) : null}
        </div>
        <div className="flex justify-end gap-2 border-t border-edge px-4 py-3">
          {connected ? (
            <button type="button" onClick={() => void session.disconnectPrinter()} className={`${BUTTON} border-edge-strong bg-surface-raised text-ink hover:bg-surface-hover`}>
              Disconnect
            </button>
          ) : (
            <button
              type="button"
              disabled={connecting || host.trim() === ""}
              onClick={connect}
              className={`${BUTTON} border-edge-strong bg-carbon-700 font-semibold text-ink-strong hover:bg-surface-hover disabled:cursor-not-allowed disabled:text-ink-disabled`}
            >
              {connecting ? "Connecting …" : "Connect"}
            </button>
          )}
          <button type="button" onClick={onClose} className={`${BUTTON} border-edge text-ink-secondary hover:bg-surface-hover`}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
