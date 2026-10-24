import type { Transport } from "./client.ts";
import { HttpTransport } from "./transport-http.ts";
import { TauriTransport } from "./transport-tauri.ts";

/** Inside the Tauri shell the engine is reached through it; in a plain browser through the dev bridge. */
export function createTransport(): Transport {
  if (typeof window !== "undefined" && "__TAURI_INTERNALS__" in window) return new TauriTransport();
  return new HttpTransport(import.meta.env.VITE_BRIDGE_URL ?? "http://127.0.0.1:4173");
}
