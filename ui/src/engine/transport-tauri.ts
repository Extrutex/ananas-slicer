// Talks to the engine through the Tauri shell (src-tauri/src/engine.rs).

import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { EngineRpcError } from "./protocol.ts";
import type { Notification, Transport } from "./client.ts";

interface RpcFailure {
  code: number;
  message: string;
}

function isRpcFailure(value: unknown): value is RpcFailure {
  return typeof value === "object" && value !== null && "code" in value && "message" in value;
}

export class TauriTransport implements Transport {
  readonly #listeners = new Set<(notification: Notification) => void>();
  #subscribed = false;

  async call(method: string, params?: object): Promise<unknown> {
    try {
      return await invoke("engine_call", { method, params: params ?? null });
    } catch (error) {
      // The shell answers an engine error as {code, message}; anything else is a failure of the shell itself.
      if (isRpcFailure(error)) throw new EngineRpcError(error.code, error.message);
      throw new Error(String(error));
    }
  }

  onNotification(handler: (notification: Notification) => void): () => void {
    this.#listeners.add(handler);
    if (!this.#subscribed) {
      this.#subscribed = true;
      void listen<Notification>("engine-notification", (event) => {
        for (const listener of this.#listeners) listener(event.payload);
      });
    }
    return () => this.#listeners.delete(handler);
  }

  defaults(): Promise<{ model: string; outputDir: string }> {
    return invoke("engine_defaults");
  }
}
