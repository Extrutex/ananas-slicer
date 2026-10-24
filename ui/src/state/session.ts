// The cockpit's state, independent of any UI framework: which printer/process/filament is selected, the
// overrides the user typed, the limits imported from a printer.cfg, what the engine resolved from that, and a
// running slice. A component renders `getState()`; every change goes through a method here, and every request
// to the engine runs one after the other so an answer is never applied over a newer one.

import { EngineClient } from "../engine/client.ts";
import {
  EngineRpcError,
  ERROR_INVALID_PARAMS,
  type CompatiblePresets,
  type EngineInfo,
  type ImportedLimits,
  type PrinterPreset,
  type ResolveResult,
  type SchemaOption,
  type SliceResult,
} from "../engine/protocol.ts";
import type { CockpitGroup } from "../cockpit/model.ts";
import { buildGroups } from "../cockpit/params.ts";

export interface SliceState {
  status: "idle" | "running" | "done" | "failed";
  percent: number;
  message: string;
  result: SliceResult | null;
  error: string | null;
}

export interface ImportedState {
  /** What the user imported, e.g. the file name. */
  label: string;
  limits: ImportedLimits;
  warnings: string[];
}

export interface CockpitState {
  phase: "loading" | "ready" | "failed";
  error: string | null;
  engine: EngineInfo | null;
  printers: PrinterPreset[];
  compatible: CompatiblePresets | null;
  selection: { printer: string; process: string; filament: string };
  schema: ReadonlyMap<string, SchemaOption>;
  resolved: ResolveResult | null;
  groups: CockpitGroup[];
  /** Overrides the engine accepted, as serialized values. */
  overrides: Readonly<Record<string, string>>;
  /** Inputs the engine rejected, key -> the engine's reason. The row keeps showing them. */
  invalid: Readonly<Record<string, string>>;
  imported: ImportedState | null;
  importError: string | null;
  model: string;
  outputDir: string;
  slice: SliceState;
}

const DEFAULT_PRINTER = "Voron 2.4 350 0.4 nozzle";
const DEFAULT_FILAMENT = "Generic PLA @System";

const IDLE_SLICE: SliceState = { status: "idle", percent: 0, message: "", result: null, error: null };

function message(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export class CockpitSession {
  readonly #client: EngineClient;
  readonly #listeners = new Set<() => void>();
  #queue: Promise<unknown> = Promise.resolve();
  #state: CockpitState = {
    phase: "loading",
    error: null,
    engine: null,
    printers: [],
    compatible: null,
    selection: { printer: "", process: "", filament: "" },
    schema: new Map(),
    resolved: null,
    groups: [],
    overrides: {},
    invalid: {},
    imported: null,
    importError: null,
    model: "",
    outputDir: "",
    slice: IDLE_SLICE,
  };

  constructor(client: EngineClient) {
    this.#client = client;
  }

  getState = (): CockpitState => this.#state;

  subscribe = (listener: () => void): (() => void) => {
    this.#listeners.add(listener);
    return () => this.#listeners.delete(listener);
  };

  #set(patch: Partial<CockpitState>): void {
    this.#state = { ...this.#state, ...patch };
    for (const listener of this.#listeners) listener();
  }

  /** Runs `task` after every earlier request has finished; a failure does not stop the queue. */
  #enqueue<T>(task: () => Promise<T>): Promise<T> {
    const run = this.#queue.then(task, task);
    this.#queue = run.catch(() => undefined);
    return run;
  }

  /** Reads the engine's presets and schema and resolves the default printer. */
  start(): Promise<void> {
    return this.#enqueue(async () => {
      try {
        const [engine, printers, options, defaults] = await Promise.all([
          this.#client.info(),
          this.#client.printers(),
          this.#client.schema(),
          this.#client.defaults(),
        ]);
        const first = printers.find((printer) => printer.name === DEFAULT_PRINTER) ?? printers[0];
        if (first === undefined) throw new Error("The engine has no printer presets.");
        this.#set({
          engine,
          printers,
          schema: new Map(options.map((option) => [option.key, option])),
          model: defaults.model,
          outputDir: defaults.outputDir,
        });
        await this.#loadPrinter(first.name);
        this.#set({ phase: "ready" });
      } catch (error) {
        this.#set({ phase: "failed", error: message(error) });
      }
    });
  }

  async #loadPrinter(printer: string): Promise<void> {
    const compatible = await this.#client.compatible(printer);
    const preset = this.#state.printers.find((candidate) => candidate.name === printer);
    const process =
      preset !== undefined && compatible.processes.includes(preset.default_process)
        ? preset.default_process
        : (compatible.processes[0] ?? "");
    const filament = compatible.filaments.includes(DEFAULT_FILAMENT) ? DEFAULT_FILAMENT : (compatible.filaments[0] ?? "");
    this.#set({ compatible, selection: { printer, process, filament }, overrides: {}, invalid: {}, imported: null, importError: null });
    await this.#resolve();
  }

  /** Resolves the current selection with its overrides and imported limits and stores the answer. */
  async #resolve(): Promise<void> {
    const { selection, overrides, imported } = this.#state;
    const resolved = await this.#client.resolve({
      printer: selection.printer,
      process: selection.process,
      filament: selection.filament,
      overrides: { ...overrides },
      limits: imported?.limits,
    });
    this.#set({ resolved, groups: buildGroups(this.#state.schema, resolved) });
  }

  selectPrinter(printer: string): Promise<void> {
    return this.#enqueue(async () => {
      try {
        await this.#loadPrinter(printer);
      } catch (error) {
        this.#set({ error: message(error) });
      }
    });
  }

  /** Switching process or filament starts from that preset's own values, so overrides are dropped. */
  selectPreset(kind: "process" | "filament", name: string): Promise<void> {
    return this.#enqueue(async () => {
      this.#set({ selection: { ...this.#state.selection, [kind]: name }, overrides: {}, invalid: {} });
      try {
        await this.#resolve();
      } catch (error) {
        this.#set({ error: message(error) });
      }
    });
  }

  /** Applies a typed value. The engine's refusal keeps the previous values and marks the row invalid. */
  setValue(key: string, value: number): Promise<void> {
    return this.#enqueue(async () => {
      const previous = this.#state.overrides;
      this.#set({ overrides: { ...previous, [key]: String(value) } });
      try {
        await this.#resolve();
        const { [key]: _cleared, ...invalid } = this.#state.invalid;
        this.#set({ invalid, error: null });
      } catch (error) {
        this.#set({ overrides: previous });
        if (error instanceof EngineRpcError && error.code === ERROR_INVALID_PARAMS) {
          this.#set({ invalid: { ...this.#state.invalid, [key]: error.message } });
        } else {
          this.#set({ error: message(error) });
        }
      }
    });
  }

  /** Drops the engine's refusal of one input, e.g. when the user reverts the row. */
  dismissInvalid(key: string): void {
    if (!(key in this.#state.invalid)) return;
    const { [key]: _dismissed, ...invalid } = this.#state.invalid;
    this.#set({ invalid });
  }

  resetOverrides(): Promise<void> {
    return this.#enqueue(async () => {
      this.#set({ overrides: {}, invalid: {} });
      try {
        await this.#resolve();
      } catch (error) {
        this.#set({ error: message(error) });
      }
    });
  }

  /** Imports the Klipper limits of a printer.cfg; they replace the profile's machine limits. */
  importPrinterCfg(text: string, label: string): Promise<void> {
    return this.#enqueue(async () => {
      try {
        const imported = await this.#client.importPrinterCfg(text);
        this.#set({
          imported: { label, limits: { ...imported.limits, source: imported.source }, warnings: imported.warnings },
          importError: null,
        });
        await this.#resolve();
      } catch (error) {
        this.#set({ importError: message(error) });
      }
    });
  }

  clearImported(): Promise<void> {
    return this.#enqueue(async () => {
      this.#set({ imported: null, importError: null });
      try {
        await this.#resolve();
      } catch (error) {
        this.#set({ error: message(error) });
      }
    });
  }

  setModel(model: string): void {
    this.#set({ model });
  }

  /** Slices the model with the current selection, overrides and limits into the output folder. */
  slice(): Promise<SliceResult | null> {
    return this.#enqueue(async () => {
      const { selection, overrides, imported, model, outputDir } = this.#state;
      const base = model.replace(/^.*[\\/]/, "").replace(/\.[^.]+$/, "") || "model";
      this.#set({ slice: { ...IDLE_SLICE, status: "running" } });
      try {
        const result = await this.#client.slice(
          {
            printer: selection.printer,
            process: selection.process,
            filament: selection.filament,
            overrides: { ...overrides },
            limits: imported?.limits,
            model,
            output: `${outputDir.replace(/[\\/]$/, "")}/${base}.gcode`,
          },
          (progress) => this.#set({ slice: { ...this.#state.slice, status: "running", ...progress } }),
        );
        this.#set({ slice: { status: "done", percent: 100, message: "Finished", result, error: null } });
        return result;
      } catch (error) {
        this.#set({ slice: { ...IDLE_SLICE, status: "failed", error: message(error) } });
        return null;
      }
    });
  }
}
