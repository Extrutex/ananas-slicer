import { useEffect, useRef, useState } from "react";
import { AckDialog } from "./AckDialog.tsx";
import { PrintDialog } from "./PrintDialog.tsx";
import { PrinterDialog } from "./PrinterDialog.tsx";
import { StressPanel } from "./StressPanel.tsx";
import { OBJECT_PREFIX, objectGroup } from "./cockpit/objectParams.ts";
import { Sidebar } from "./cockpit/Sidebar.tsx";
import type { SliceResult } from "./engine/protocol.ts";
import type { CockpitSession } from "./state/session.ts";
import { useSession } from "./state/useSession.ts";
import { fitsBed } from "./viewer/bed.ts";
import { Viewer } from "./viewer/Viewer.tsx";
import { createPlatform, type Platform } from "./platform.ts";

const SELECT_CLASS =
  "h-target max-w-[220px] truncate rounded border border-edge-field bg-surface-field px-1.5 font-data text-ks-table text-ink-strong";

function SliceSummary({ result }: { result: SliceResult }) {
  const minutes = Math.floor(result.time_s / 60);
  const seconds = Math.round(result.time_s % 60);
  return (
    <dl className="m-0 grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 font-data text-ks-table" data-testid="slice-summary">
      <dt className="text-ink-muted">G-code</dt>
      <dd className="m-0 truncate text-ink-strong" title={result.gcode}>
        {result.gcode}
      </dd>
      <dt className="text-ink-muted">Time</dt>
      <dd className="m-0 text-ink-strong">
        {minutes} min {seconds} s
      </dd>
      <dt className="text-ink-muted">Layers</dt>
      <dd className="m-0 text-ink-strong">{result.layers}</dd>
      <dt className="text-ink-muted">Filament</dt>
      <dd className="m-0 text-ink-strong">
        {(result.filament_mm / 1000).toFixed(2)} m {"·"} {result.filament_g.toFixed(1)} g
      </dd>
    </dl>
  );
}

const defaultPlatform = createPlatform();

export function App({ session, platform = defaultPlatform }: { session: CockpitSession; platform?: Platform }) {
  const state = useSession(session);
  const fileInput = useRef<HTMLInputElement>(null);
  const [acknowledging, setAcknowledging] = useState(false);
  const [printerOpen, setPrinterOpen] = useState(false);
  const [confirmingPrint, setConfirmingPrint] = useState(false);
  const [saved, setSaved] = useState<{ path: string } | { error: string } | null>(null);

  // While a print runs (or has just been started) the printer's state is read every two seconds.
  const jobState = state.printer.job?.state;
  const watching = state.printer.status === "connected" && (jobState === "printing" || jobState === "paused" || state.upload.printStarted);
  useEffect(() => {
    if (!watching) return;
    const timer = window.setInterval(() => void session.refreshPrinter(), 2000);
    return () => window.clearInterval(timer);
  }, [watching, session]);

  if (state.phase === "loading") {
    return <p className="p-6 font-data text-ks-ui text-ink-muted">Starting the engine {"…"}</p>;
  }
  if (state.phase === "failed") {
    return (
      <div role="alert" className="p-6 font-ui text-ks-ui text-signal-limit">
        <h1 className="m-0 text-ks-title">The engine did not start</h1>
        <p className="font-data text-ks-table">{state.error}</p>
      </div>
    );
  }

  const exceeded = state.resolved?.violations.filter((violation) => violation.state === "exceeded") ?? [];
  const running = state.slice.status === "running";
  const selectedObject = state.selected !== null ? state.objects[state.selected] : undefined;
  const outside = state.objects.some((object) => !fitsBed(object.box, state.bed));
  const groups = selectedObject ? [objectGroup(selectedObject), ...state.groups] : state.groups;
  const limitsSource = state.imported
    ? `${state.resolved?.limits?.source ?? "printer.cfg"} · ${state.imported.label}`
    : "profile defaults";

  const startSlice = () => {
    if (exceeded.length > 0) setAcknowledging(true);
    else void session.slice();
  };

  return (
    <div className="flex h-full flex-col bg-surface-app font-ui text-ks-ui text-ink">
      <header className="flex h-11 shrink-0 items-center gap-3 border-b border-edge bg-surface-chrome px-3">
        <span className="font-semibold tracking-[0.08em] text-ink-strong">ANANAS SLICER</span>
        <label className="flex items-center gap-1.5 text-ks-table text-ink-muted">
          Printer
          <select
            aria-label="Printer"
            className={SELECT_CLASS}
            value={state.selection.printer}
            onChange={(event) => void session.selectPrinter(event.target.value)}
          >
            {state.printers.map((printer) => (
              <option key={printer.name} value={printer.name}>
                {printer.name}
              </option>
            ))}
          </select>
        </label>
        <label className="flex items-center gap-1.5 text-ks-table text-ink-muted">
          Process
          <select
            aria-label="Process"
            className={SELECT_CLASS}
            value={state.selection.process}
            onChange={(event) => void session.selectPreset("process", event.target.value)}
          >
            {state.compatible?.processes.map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
        </label>
        <label className="flex items-center gap-1.5 text-ks-table text-ink-muted">
          Filament
          <select
            aria-label="Filament"
            className={SELECT_CLASS}
            value={state.selection.filament}
            onChange={(event) => void session.selectPreset("filament", event.target.value)}
          >
            {state.compatible?.filaments.map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
        </label>

        <input
          ref={fileInput}
          type="file"
          accept=".cfg,text/plain"
          hidden
          data-testid="printer-cfg-input"
          onChange={(event) => {
            const file = event.target.files?.[0];
            if (file) void file.text().then((text) => session.importPrinterCfg(text, file.name));
            event.target.value = "";
          }}
        />
        <button
          type="button"
          onClick={() => fileInput.current?.click()}
          className="h-target rounded border border-edge-strong bg-surface-raised px-2 text-ks-table text-ink hover:bg-surface-hover"
        >
          Import printer.cfg {"…"}
        </button>
        <button
          type="button"
          aria-label="Printer connection"
          onClick={() => setPrinterOpen(true)}
          className="flex h-target items-center gap-1.5 rounded border border-edge-strong bg-surface-raised px-2 text-ks-table text-ink hover:bg-surface-hover"
        >
          <span
            aria-hidden="true"
            className={`inline-block h-2 w-2 rounded-full ${state.printer.status === "connected" ? "bg-signal-nominal" : state.printer.status === "failed" ? "bg-signal-limit" : "bg-steel-400"}`}
          />
          {state.printer.status === "connected" ? state.printer.host.replace(/^https?:\/\//, "") : "Connect printer \u2026"}
        </button>
        {state.imported ? (
          <button
            type="button"
            onClick={() => void session.clearImported()}
            className="h-target rounded border border-edge px-2 text-ks-table text-ink-muted hover:bg-surface-hover"
          >
            Use profile limits
          </button>
        ) : null}
        <button
          type="button"
          aria-pressed={state.stress.open}
          disabled={state.objects.length === 0}
          onClick={() => (state.stress.open ? session.closeStress() : session.openStress())}
          className={`h-target rounded border px-2 text-ks-table disabled:cursor-not-allowed disabled:text-ink-disabled ${state.stress.open ? "border-edge-strong bg-carbon-700 text-ink-strong" : "border-edge-strong bg-surface-raised text-ink hover:bg-surface-hover"}`}
        >
          Kraftverteilung
        </button>
        {Object.keys(state.overrides).length > 0 ? (
          <button
            type="button"
            onClick={() => void session.resetOverrides()}
            className="h-target rounded border border-edge px-2 text-ks-table text-ink-muted hover:bg-surface-hover"
          >
            Reset {Object.keys(state.overrides).length} changed
          </button>
        ) : null}

        <button
          type="button"
          disabled={running || outside || state.objects.length === 0}
          title={outside ? "An object is outside the print area" : undefined}
          onClick={startSlice}
          className="ml-auto h-target rounded border border-edge-strong bg-carbon-700 px-3 font-semibold text-ink-strong hover:bg-surface-hover disabled:cursor-not-allowed disabled:text-ink-disabled"
        >
          Slice plate{exceeded.length > 0 ? ` (${exceeded.length} LIMIT)` : ""}
        </button>
      </header>

      <div className="flex min-h-0 flex-1">
        <Sidebar
          groups={groups}
          rejected={state.invalid}
          onValueChange={(key, value) => void (key.startsWith(OBJECT_PREFIX) ? session.setObjectValue(key, value) : session.setValue(key, value))}
          onDismiss={(key) => session.dismissInvalid(key)}
          machineLabel={state.selection.printer}
          configSource={limitsSource}
          extra={
            selectedObject ? (
              <label className="flex items-center gap-2 border-b border-edge px-3 py-1.5 font-data text-ks-table text-ink-muted">
                <input type="checkbox" checked={state.uniformScale} onChange={(event) => session.setUniformScale(event.target.checked)} />
                Uniform scale
              </label>
            ) : null
          }
        />

        <main className="flex min-w-0 flex-1 flex-col bg-surface-viewport">
          <div className="relative flex min-h-0 flex-1 flex-col">
            <Viewer session={session} state={state} />
            <StressPanel session={session} state={state} />
          </div>
          <section aria-label="Slice" className="border-t border-edge bg-surface-panel px-4 py-3">
            <div className="flex items-center gap-3">
              <label className="flex flex-1 items-center gap-2 text-ks-table text-ink-muted">
                Model
                <input
                  aria-label="Model file"
                  type="text"
                  spellCheck={false}
                  value={state.model}
                  onChange={(event) => session.setModel(event.target.value)}
                  onKeyDown={(event) => event.key === "Enter" && void session.loadModel(state.model)}
                  className="h-target min-w-0 flex-1 rounded border border-edge-field bg-surface-field px-2 font-data text-ks-table text-ink-strong"
                />
              </label>
              {platform.native ? (
                <button
                  type="button"
                  onClick={() => void platform.pickModel().then((path) => (path === null ? undefined : session.loadModel(path)))}
                  className="h-target rounded border border-edge-strong bg-surface-raised px-3 text-ks-table text-ink hover:bg-surface-hover"
                >
                  Choose {"\u2026"}
                </button>
              ) : null}
              <button
                type="button"
                onClick={() => void session.loadModel(state.model)}
                className="h-target rounded border border-edge-strong bg-surface-raised px-3 text-ks-table text-ink hover:bg-surface-hover"
              >
                Open
              </button>
              {state.modelError ? (
                <span role="alert" className="max-w-[40%] truncate font-data text-ks-table text-signal-limit" title={state.modelError}>
                  {state.modelError}
                </span>
              ) : null}
            </div>
            {state.slice.status !== "idle" ? (
              <div className="mt-3" data-testid="slice-status" data-status={state.slice.status}>
                {running ? (
                  <div>
                    <div
                      role="progressbar"
                      aria-valuemin={0}
                      aria-valuemax={100}
                      aria-valuenow={state.slice.percent}
                      className="h-1.5 w-full bg-surface-field"
                    >
                      <div className="h-full bg-signal-path" style={{ width: `${state.slice.percent}%` }} />
                    </div>
                    <p className="m-0 mt-1 truncate font-data text-ks-table text-ink-muted">
                      {state.slice.percent} % {"·"} {state.slice.message}
                    </p>
                  </div>
                ) : null}
                {state.slice.status === "done" && state.slice.result ? (
                  <>
                    <SliceSummary result={state.slice.result} />
                    <div className="mt-3 flex items-center gap-2">
                      <button
                        type="button"
                        disabled={state.printer.status !== "connected" || state.upload.status === "uploading"}
                        title={state.printer.status !== "connected" ? "Connect a printer first" : undefined}
                        onClick={() => void session.sendToPrinter(false)}
                        className="h-target rounded border border-edge-strong bg-surface-raised px-3 text-ks-table text-ink hover:bg-surface-hover disabled:cursor-not-allowed disabled:text-ink-disabled"
                      >
                        Send to printer
                      </button>
                      <button
                        type="button"
                        disabled={state.printer.status !== "connected" || state.upload.status === "uploading" || state.printer.job?.state === "printing"}
                        title={state.printer.status !== "connected" ? "Connect a printer first" : undefined}
                        onClick={() => setConfirmingPrint(true)}
                        className="h-target rounded border border-edge-strong bg-carbon-700 px-3 text-ks-table font-semibold text-ink-strong hover:bg-surface-hover disabled:cursor-not-allowed disabled:text-ink-disabled"
                      >
                        Send and print {"\u2026"}
                      </button>
                      {platform.native ? (
                        <button
                          type="button"
                          onClick={() => {
                            const result = state.slice.result;
                            if (!result) return;
                            const name = result.gcode.replace(/^.*[\\/]/, "");
                            void platform
                              .saveGcode(result.gcode, name)
                              .then((path) => path !== null && setSaved({ path }))
                              .catch((error: unknown) => setSaved({ error: error instanceof Error ? error.message : String(error) }));
                          }}
                          className="h-target rounded border border-edge-strong bg-surface-raised px-3 text-ks-table text-ink hover:bg-surface-hover"
                        >
                          Save G-code {"\u2026"}
                        </button>
                      ) : null}
                      {saved ? (
                        <span data-testid="saved" className={`font-data text-ks-table ${"error" in saved ? "text-signal-limit" : "text-ink-muted"}`}>
                          {"path" in saved ? `Saved to ${saved.path}` : saved.error}
                        </span>
                      ) : null}
                      <span data-testid="upload-status" data-status={state.upload.status} className={`font-data text-ks-table ${state.upload.status === "failed" ? "text-signal-limit" : "text-ink-muted"}`}>
                        {state.upload.status === "uploading" ? "Uploading \u2026" : null}
                        {state.upload.status === "done" ? `Stored as ${state.upload.name}${state.upload.printStarted ? " \u00b7 print started" : ""}` : null}
                        {state.upload.status === "failed" ? state.upload.error : null}
                      </span>
                    </div>
                  </>
                ) : null}
                {state.printer.status === "connected" && state.printer.job && state.printer.job.state !== "standby" ? (
                  <div className="mt-3" data-testid="job" data-state={state.printer.job.state}>
                    <div className="flex items-center gap-3 font-data text-ks-table text-ink-secondary">
                      <span className="text-ink-strong">{state.printer.job.state}</span>
                      <span className="truncate">{state.printer.job.filename}</span>
                      <span className="ml-auto">{Math.round(state.printer.job.progress * 100)} %</span>
                      {state.printer.job.state === "printing" || state.printer.job.state === "paused" ? (
                        <button
                          type="button"
                          onClick={() => void session.cancelPrint()}
                          className="h-target rounded border border-edge-strong bg-surface-raised px-2 text-ks-table text-ink hover:bg-surface-hover"
                        >
                          Cancel print
                        </button>
                      ) : null}
                    </div>
                    <div className="mt-1 h-1.5 w-full bg-surface-field" role="progressbar" aria-label="Print progress" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(state.printer.job.progress * 100)}>
                      <div className="h-full bg-signal-nominal" style={{ width: `${Math.round(state.printer.job.progress * 100)}%` }} />
                    </div>
                  </div>
                ) : null}
                {state.slice.status === "failed" ? (
                  <p role="alert" className="m-0 font-data text-ks-table text-signal-limit">
                    {state.slice.error}
                  </p>
                ) : null}
              </div>
            ) : null}
          </section>
        </main>
      </div>

      <footer className="flex h-6 shrink-0 items-center gap-4 border-t border-edge bg-surface-chrome px-3 font-data text-ks-micro text-ink-muted">
        <span>engine {state.engine?.version}</span>
        <span>limits {"\u00b7"} {limitsSource}</span>
        {state.printer.status === "connected" ? (
          <span data-testid="printer-footer">
            printer {"\u00b7"} {state.printer.klippyState || "?"}
            {state.printer.job ? ` \u00b7 ${state.printer.job.state}` : ""}
          </span>
        ) : null}
        <span className="truncate">
          {state.selection.process} {"·"} {state.selection.filament}
        </span>
        {state.error ? <span className="ml-auto truncate text-signal-limit">{state.error}</span> : null}
        {state.importError ? <span className="ml-auto truncate text-signal-limit">import: {state.importError}</span> : null}
      </footer>

      {printerOpen ? <PrinterDialog session={session} printer={state.printer} onClose={() => setPrinterOpen(false)} /> : null}
      {confirmingPrint && state.slice.result ? (
        <PrintDialog
          host={state.printer.host}
          result={state.slice.result}
          fileName={state.slice.result.gcode.replace(/^.*[\\/]/, "")}
          onCancel={() => setConfirmingPrint(false)}
          onConfirm={() => {
            setConfirmingPrint(false);
            void session.sendToPrinter(true);
          }}
        />
      ) : null}

      {acknowledging ? (
        <AckDialog
          violations={exceeded}
          onCancel={() => setAcknowledging(false)}
          onConfirm={() => {
            setAcknowledging(false);
            void session.slice();
          }}
        />
      ) : null}
    </div>
  );
}
