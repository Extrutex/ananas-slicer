// Talks to the dev bridge (dev-bridge.ts) from a plain browser.

import { EngineRpcError } from "./protocol.ts";
import type { Notification, Transport } from "./client.ts";

export class HttpTransport implements Transport {
  readonly #base: string;
  readonly #listeners = new Set<(notification: Notification) => void>();
  #events: EventSource | null = null;

  constructor(base: string) {
    this.#base = base.replace(/\/$/, "");
  }

  async call(method: string, params?: object): Promise<unknown> {
    const response = await fetch(`${this.#base}/rpc`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ method, params }),
    });
    const body = (await response.json()) as { result?: unknown; error?: { code: number; message: string } };
    if (body.error !== undefined) throw new EngineRpcError(body.error.code, body.error.message);
    return body.result;
  }

  onNotification(handler: (notification: Notification) => void): () => void {
    this.#listeners.add(handler);
    if (this.#events === null) {
      this.#events = new EventSource(`${this.#base}/events`);
      this.#events.onmessage = (event: MessageEvent<string>) => {
        const notification = JSON.parse(event.data) as Notification;
        for (const listener of this.#listeners) listener(notification);
      };
    }
    return () => this.#listeners.delete(handler);
  }

  async defaults(): Promise<{ model: string; outputDir: string }> {
    return (await (await fetch(`${this.#base}/defaults`)).json()) as { model: string; outputDir: string };
  }
}
