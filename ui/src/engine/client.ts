import type {
  BoundsResult,
  CompatiblePresets,
  EngineInfo,
  LimitsImportResult,
  ModelLoadResult,
  ObjectTransform,
  PreviewChunkJson,
  PreviewSummary,
  PrinterConnectResult,
  PrinterPreset,
  PrinterStatus,
  ResolveResult,
  SchemaOption,
  Selection,
  SliceProgress,
  SliceRequest,
  SliceResult,
  UploadResult,
} from "./protocol.ts";

export interface Notification {
  method: string;
  params: unknown;
}

/** How requests reach the engine: the Tauri shell, the dev bridge, or a child process in tests. */
export interface Transport {
  /** Resolves with the result, rejects with an EngineRpcError for an error answer. */
  call(method: string, params?: object): Promise<unknown>;
  onNotification(handler: (notification: Notification) => void): () => void;
  /** Default paths of the host: a sample model and a folder for the G-code. */
  defaults(): Promise<{ model: string; outputDir: string }>;
}

export class EngineClient {
  readonly #transport: Transport;

  constructor(transport: Transport) {
    this.#transport = transport;
  }

  defaults() {
    return this.#transport.defaults();
  }

  async info(): Promise<EngineInfo> {
    return (await this.#transport.call("engine.info")) as EngineInfo;
  }

  async printers(): Promise<PrinterPreset[]> {
    const result = (await this.#transport.call("presets.list")) as { printers: PrinterPreset[] };
    return result.printers;
  }

  async compatible(printer: string): Promise<CompatiblePresets> {
    return (await this.#transport.call("presets.list", { printer })) as CompatiblePresets;
  }

  async schema(): Promise<SchemaOption[]> {
    const result = (await this.#transport.call("config.schema")) as { options: SchemaOption[] };
    return result.options;
  }

  async resolve(selection: Selection): Promise<ResolveResult> {
    return (await this.#transport.call("config.resolve", selection)) as ResolveResult;
  }

  async importPrinterCfg(text: string): Promise<LimitsImportResult> {
    return (await this.#transport.call("limits.import", { printer_cfg: text })) as LimitsImportResult;
  }

  async loadModel(path: string): Promise<ModelLoadResult> {
    return (await this.#transport.call("model.load", { path })) as ModelLoadResult;
  }

  async bounds(path: string, transforms: ObjectTransform[]): Promise<BoundsResult> {
    return (await this.#transport.call("model.bounds", { path, transforms })) as BoundsResult;
  }

  async previewLoad(gcode: string): Promise<PreviewSummary> {
    return (await this.#transport.call("preview.load", { gcode })) as PreviewSummary;
  }

  async previewChunk(gcode: string, layerFrom: number, layerTo: number, travel: boolean): Promise<PreviewChunkJson> {
    return (await this.#transport.call("preview.chunk", {
      gcode,
      layer_from: layerFrom,
      layer_to: layerTo,
      travel,
    })) as PreviewChunkJson;
  }

  // --- the printer host (Moonraker or FLUXRAKER) ---

  async connectPrinter(host: string, apiKey: string): Promise<PrinterConnectResult> {
    return (await this.#transport.call("moonraker.connect", { host, api_key: apiKey })) as PrinterConnectResult;
  }

  async uploadGcode(host: string, apiKey: string, gcode: string, print: boolean): Promise<UploadResult> {
    return (await this.#transport.call("moonraker.upload", { host, api_key: apiKey, gcode, print })) as UploadResult;
  }

  async startPrint(host: string, apiKey: string, name: string): Promise<void> {
    await this.#transport.call("moonraker.start", { host, api_key: apiKey, name });
  }

  async cancelPrint(host: string, apiKey: string): Promise<void> {
    await this.#transport.call("moonraker.cancel", { host, api_key: apiKey });
  }

  async printerStatus(host: string, apiKey: string): Promise<PrinterStatus> {
    return (await this.#transport.call("moonraker.status", { host, api_key: apiKey })) as PrinterStatus;
  }

  /** One slice at a time; `onProgress` receives the engine's `slice.progress` notifications meanwhile. */
  async slice(request: SliceRequest, onProgress: (progress: SliceProgress) => void): Promise<SliceResult> {
    const stop = this.#transport.onNotification((notification) => {
      if (notification.method === "slice.progress") {
        const params = notification.params as { percent: number; message: string };
        onProgress({ percent: params.percent, message: params.message });
      }
    });
    try {
      return (await this.#transport.call("slice", request)) as SliceResult;
    } finally {
      stop();
    }
  }
}
