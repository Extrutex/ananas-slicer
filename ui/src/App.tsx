import { useRef, useState } from "react";
import { AckDialog } from "./AckDialog.tsx";
import { OBJECT_PREFIX, objectGroup } from "./cockpit/objectParams.ts";
import { Sidebar } from "./cockpit/Sidebar.tsx";
import type { SliceResult } from "./engine/protocol.ts";
import type { CockpitSession } from "./state/session.ts";
import { useSession } from "./state/useSession.ts";
import { fitsBed } from "./viewer/bed.ts";
import { Viewer } from "./viewer/Viewer.tsx";

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

export function App({ session }: { session: CockpitSession }) {
  const state = useSession(session);
  const fileInput = useRef<HTMLInputElement>(null);
  const [acknowledging, setAcknowledging] = useState(false);

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
        {state.imported ? (
          <button
            type="button"
            onClick={() => void session.clearImported()}
            className="h-target rounded border border-edge px-2 text-ks-table text-ink-muted hover:bg-surface-hover"
          >
            Use profile limits
          </button>
        ) : null}
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
          <Viewer session={session} state={state} />
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
                {state.slice.status === "done" && state.slice.result ? <SliceSummary result={state.slice.result} /> : null}
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
        <span>limits {"·"} {limitsSource}</span>
        <span className="truncate">
          {state.selection.process} {"·"} {state.selection.filament}
        </span>
        {state.error ? <span className="ml-auto truncate text-signal-limit">{state.error}</span> : null}
        {state.importError ? <span className="ml-auto truncate text-signal-limit">import: {state.importError}</span> : null}
      </footer>

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
