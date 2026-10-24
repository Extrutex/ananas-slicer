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
import { axisIndex, parseObjectId } from "../cockpit/objectParams.ts";
import { buildGroups } from "../cockpit/params.ts";
import { fitsBed, parseBed, type Bed } from "../viewer/bed.ts";
import type { Quantity } from "../viewer/heatmap.ts";
import { objectsFromModel, toEngineTransform, type SceneObject, type Transform } from "../viewer/objects.ts";
import { mergeChunks, type PreviewData } from "../viewer/preview.ts";

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

export type ViewMode = "solid" | "mesh" | "inspect";

export interface PreviewState {
  status: "none" | "loading" | "ready" | "failed";
  data: PreviewData | null;
  error: string | null;
  quantity: Quantity;
  /** Inclusive layer indices shown. */
  layerMin: number;
  layerMax: number;
  showTravel: boolean;
  /** Vertical gap added per layer, mm. */
  explodeLayers: number;
}

export interface ViewState {
  /** What the viewport shows: the model on the bed or the sliced G-code. */
  target: "model" | "gcode";
  mode: ViewMode;
  /** 0..1: how far the model's parts are pulled apart. */
  explode: number;
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
  modelError: string | null;
  outputDir: string;
  bed: Bed;
  objects: SceneObject[];
  selected: number | null;
  uniformScale: boolean;
  view: ViewState;
  preview: PreviewState;
  slice: SliceState;
}

const DEFAULT_PRINTER = "Voron 2.4 350 0.4 nozzle";
const DEFAULT_FILAMENT = "Generic PLA @System";

const IDLE_SLICE: SliceState = { status: "idle", percent: 0, message: "", result: null, error: null };

const NO_PREVIEW: PreviewState = {
  status: "none",
  data: null,
  error: null,
  quantity: "speed",
  layerMin: 0,
  layerMax: 0,
  showTravel: false,
  explodeLayers: 0,
};

/** Layers fetched per request; the toolpaths of a layer range arrive as typed arrays. */
const PREVIEW_CHUNK_LAYERS = 25;

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
    modelError: null,
    outputDir: "",
    bed: parseBed(undefined, undefined),
    objects: [],
    selected: null,
    uniformScale: true,
    view: { target: "model", mode: "solid", explode: 0 },
    preview: NO_PREVIEW,
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
        await this.#loadModelFile(defaults.model);
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
    this.#set({
      resolved,
      groups: buildGroups(this.#state.schema, resolved),
      bed: parseBed(resolved.config.printable_area, resolved.config.printable_height),
    });
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

  // --- model on the bed -------------------------------------------------------------------------

  /** Loads a model file and puts its objects in the middle of the bed. */
  loadModel(path: string): Promise<void> {
    return this.#enqueue(() => this.#loadModelFile(path));
  }

  async #loadModelFile(path: string): Promise<void> {
    try {
      const model = await this.#client.loadModel(path);
      const placed = objectsFromModel(model, this.#state.bed.center);
      // Show the objects only once the engine has confirmed where they stand, so nothing is drawn (or framed)
      // at a position that is about to change.
      const bounds = await this.#client.bounds(path, placed.map((object) => toEngineTransform(object.transform)));
      const objects = placed.map((object, index) => {
        const answer = bounds.objects[index];
        return answer ? { ...object, box: answer.bbox, offset: answer.offset } : object;
      });
      this.#set({ model: path, modelError: null, objects, selected: objects.length === 1 ? 0 : null, preview: NO_PREVIEW, view: { ...this.#state.view, target: "model" } });
    } catch (error) {
      this.#set({ modelError: message(error), objects: [], selected: null });
    }
  }

  /** Asks the engine for every object's box and offset under its current transform. */
  async #refreshBounds(): Promise<void> {
    const { model, objects } = this.#state;
    if (objects.length === 0) return;
    const bounds = await this.#client.bounds(model, objects.map((object) => toEngineTransform(object.transform)));
    this.#set({
      objects: this.#state.objects.map((object, index) => {
        const answer = bounds.objects[index];
        return answer ? { ...object, box: answer.bbox, offset: answer.offset } : object;
      }),
    });
  }

  /** The object that is moved by a drag stays where the pointer puts it until the engine has confirmed the placement. */
  moveObject(index: number, position: [number, number]): void {
    this.#set({
      objects: this.#state.objects.map((object, at) => {
        if (at !== index) return object;
        const delta: [number, number] = [position[0] - object.transform.position[0], position[1] - object.transform.position[1]];
        return {
          ...object,
          transform: { ...object.transform, position },
          offset: [object.offset[0] + delta[0], object.offset[1] + delta[1], object.offset[2]],
          box: {
            min: [object.box.min[0] + delta[0], object.box.min[1] + delta[1], object.box.min[2]],
            max: [object.box.max[0] + delta[0], object.box.max[1] + delta[1], object.box.max[2]],
          },
        };
      }),
    });
  }

  /** The drag ended: let the engine confirm the final placement. */
  commitObject(): Promise<void> {
    return this.#enqueue(async () => {
      try {
        await this.#refreshBounds();
      } catch (error) {
        this.#set({ error: message(error) });
      }
    });
  }

  selectObject(index: number | null): void {
    this.#set({ selected: index });
  }

  setUniformScale(uniform: boolean): void {
    this.#set({ uniformScale: uniform });
  }

  /** Applies an edit of an "object.<field>.<axis>" row to the selected object. Scale rows are percent. */
  setObjectValue(id: string, value: number): Promise<void> {
    return this.#enqueue(async () => {
      const parsed = parseObjectId(id);
      const index = this.#state.selected;
      if (parsed === null || index === null || this.#state.objects[index] === undefined) return;
      const axis = axisIndex(parsed.axis);
      const current = this.#state.objects[index]!.transform;
      const next: Transform = { position: [...current.position], rotation: [...current.rotation], scale: [...current.scale] };
      if (parsed.field === "position" && axis < 2) next.position[axis] = value;
      else if (parsed.field === "rotation") next.rotation[axis] = value;
      else if (parsed.field === "scale") {
        const factor = value / 100;
        if (this.#state.uniformScale) next.scale = [factor, factor, factor];
        else next.scale[axis] = factor;
      }
      this.#set({ objects: this.#state.objects.map((object, at) => (at === index ? { ...object, transform: next } : object)) });
      try {
        await this.#refreshBounds();
      } catch (error) {
        this.#set({ objects: this.#state.objects.map((object, at) => (at === index ? { ...object, transform: current } : object)) });
        this.#set({ invalid: { ...this.#state.invalid, [id]: message(error) } });
      }
    });
  }

  /** Whether every object stands on the bed. */
  get fitsBed(): boolean {
    return this.#state.objects.every((object) => fitsBed(object.box, this.#state.bed));
  }

  setViewMode(mode: ViewMode): void {
    this.#set({ view: { ...this.#state.view, mode } });
  }

  setViewTarget(target: "model" | "gcode"): void {
    this.#set({ view: { ...this.#state.view, target } });
  }

  setExplode(explode: number): void {
    this.#set({ view: { ...this.#state.view, explode } });
  }

  // --- G-code preview ---------------------------------------------------------------------------

  /** Reads a G-code file's toolpaths layer range by layer range and shows them. */
  loadPreview(gcode: string): Promise<void> {
    return this.#enqueue(async () => {
      this.#set({ preview: { ...NO_PREVIEW, status: "loading" } });
      try {
        const summary = await this.#client.previewLoad(gcode);
        const chunks = [];
        for (let from = 0; from < summary.layers.length; from += PREVIEW_CHUNK_LAYERS) {
          chunks.push(await this.#client.previewChunk(gcode, from, Math.min(from + PREVIEW_CHUNK_LAYERS, summary.layers.length) - 1, true));
        }
        const data = mergeChunks(summary, chunks);
        this.#set({
          preview: { ...NO_PREVIEW, status: "ready", data, layerMin: 0, layerMax: Math.max(0, summary.layers.length - 1) },
          view: { ...this.#state.view, target: "gcode" },
        });
      } catch (error) {
        this.#set({ preview: { ...NO_PREVIEW, status: "failed", error: message(error) } });
      }
    });
  }

  setPreview(patch: Partial<Pick<PreviewState, "quantity" | "layerMin" | "layerMax" | "showTravel" | "explodeLayers">>): void {
    this.#set({ preview: { ...this.#state.preview, ...patch } });
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
            transforms: this.#state.objects.length > 0 ? this.#state.objects.map((object) => toEngineTransform(object.transform)) : undefined,
          },
          (progress) => this.#set({ slice: { ...this.#state.slice, status: "running", ...progress } }),
        );
        this.#set({ slice: { status: "done", percent: 100, message: "Finished", result, error: null } });
        void this.loadPreview(result.gcode);
        return result;
      } catch (error) {
        this.#set({ slice: { ...IDLE_SLICE, status: "failed", error: message(error) } });
        return null;
      }
    });
  }
}
