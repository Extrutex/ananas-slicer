import type {
  CompatiblePresets,
  EngineInfo,
  LimitsImportResult,
  PrinterPreset,
  ResolveResult,
  SchemaOption,
  Selection,
  SliceProgress,
  SliceRequest,
  SliceResult,
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
