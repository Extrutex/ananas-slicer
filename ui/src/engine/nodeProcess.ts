// Runs ananas-engine as a child process and speaks its line-based JSON-RPC protocol. Used by the tests and
// by the dev bridge (Node only; the Tauri shell has its own implementation in Rust).

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { createInterface } from "node:readline";
import { EngineRpcError } from "./protocol.ts";
import type { Notification, Transport } from "./client.ts";

interface Pending {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
}

export interface NodeEngineOptions {
  binary: string;
  profiles: string;
  /** Limits the profile load to one vendor (plus the filament library); empty loads all. */
  vendor?: string;
  defaults: { model: string; outputDir: string };
}

export class NodeEngineProcess implements Transport {
  readonly #child: ChildProcessWithoutNullStreams;
  readonly #pending = new Map<number, Pending>();
  readonly #listeners = new Set<(notification: Notification) => void>();
  readonly #defaults: { model: string; outputDir: string };
  #nextId = 1;
  #closed = false;

  constructor(options: NodeEngineOptions) {
    this.#defaults = options.defaults;
    const args = ["--profiles", options.profiles];
    if (options.vendor) args.push("--vendor", options.vendor);
    this.#child = spawn(options.binary, args, { stdio: ["pipe", "pipe", "pipe"] });
    this.#child.stderr.resume(); // the engine logs to stderr; not needed here
    createInterface({ input: this.#child.stdout }).on("line", (line) => this.#receive(line));
    this.#child.on("exit", (code) => {
      this.#closed = true;
      for (const pending of this.#pending.values()) {
        pending.reject(new Error(`The engine exited (code ${code}).`));
      }
      this.#pending.clear();
    });
  }

  #receive(line: string): void {
    const message = JSON.parse(line) as {
      id?: number;
      method?: string;
      params?: unknown;
      result?: unknown;
      error?: { code: number; message: string };
    };
    if (message.id === undefined || message.id === null) {
      if (message.method !== undefined) {
        for (const listener of this.#listeners) listener({ method: message.method, params: message.params });
      }
      return;
    }
    const pending = this.#pending.get(message.id);
    if (pending === undefined) return;
    this.#pending.delete(message.id);
    if (message.error !== undefined) pending.reject(new EngineRpcError(message.error.code, message.error.message));
    else pending.resolve(message.result);
  }

  call(method: string, params?: object): Promise<unknown> {
    if (this.#closed) return Promise.reject(new Error("The engine has exited."));
    const id = this.#nextId++;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
    });
  }

  onNotification(handler: (notification: Notification) => void): () => void {
    this.#listeners.add(handler);
    return () => this.#listeners.delete(handler);
  }

  defaults(): Promise<{ model: string; outputDir: string }> {
    return Promise.resolve(this.#defaults);
  }

  /** Ends the engine and waits for it to exit. */
  close(): Promise<void> {
    if (this.#closed) return Promise.resolve();
    return new Promise((resolve) => {
      this.#child.once("exit", () => resolve());
      this.#child.stdin.end();
    });
  }
}
