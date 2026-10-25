// Dev bridge: serves the engine over HTTP so the UI can run in a plain browser (npm run dev + npm run bridge).
// POST /rpc {method, params} -> {result} | {error}; GET /events streams the engine's notifications (SSE);
// GET /defaults -> {model, outputDir}. Listens on localhost only. The Tauri shell does not use it.
//
//   node dev-bridge.ts            (ANANAS_ENGINE_BIN, ANANAS_PROFILES_DIR, ANANAS_BRIDGE_PORT override the defaults)

import { mkdtempSync } from "node:fs";
import { createServer, type ServerResponse } from "node:http";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { EngineRpcError } from "./src/engine/protocol.ts";
import { NodeEngineProcess } from "./src/engine/nodeProcess.ts";

const repo = resolve(import.meta.dirname, "..");
const port = Number(process.env.ANANAS_BRIDGE_PORT ?? 4173);
const engine = new NodeEngineProcess({
  binary: process.env.ANANAS_ENGINE_BIN ?? join(repo, "build/mini/src/engine/Release/ananas-engine.app/Contents/MacOS/ananas-engine"),
  profiles: process.env.ANANAS_PROFILES_DIR ?? join(repo, "resources/profiles"),
  vendor: process.env.ANANAS_VENDOR ?? "Voron",
  defaults: { model: join(repo, "tests/data/20mm_cube.obj"), outputDir: mkdtempSync(join(tmpdir(), "ananas-bridge-")) },
});

const streams = new Set<ServerResponse>();
engine.onNotification((notification) => {
  for (const stream of streams) stream.write(`data: ${JSON.stringify(notification)}\n\n`);
});

function cors(response: ServerResponse): void {
  response.setHeader("Access-Control-Allow-Origin", "*");
  response.setHeader("Access-Control-Allow-Headers", "content-type");
}

const server = createServer((request, response) => {
  cors(response);
  if (request.method === "OPTIONS") {
    response.writeHead(204).end();
    return;
  }
  if (request.method === "GET" && request.url === "/defaults") {
    engine.defaults().then((defaults) => response.writeHead(200, { "content-type": "application/json" }).end(JSON.stringify(defaults)));
    return;
  }
  if (request.method === "GET" && request.url === "/events") {
    response.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-cache" });
    response.write(": connected\n\n");
    streams.add(response);
    request.on("close", () => streams.delete(response));
    return;
  }
  if (request.method === "POST" && request.url === "/rpc") {
    const chunks: Buffer[] = [];
    request.on("data", (chunk: Buffer) => chunks.push(chunk));
    request.on("end", () => {
      const send = (status: number, body: unknown) => response.writeHead(status, { "content-type": "application/json" }).end(JSON.stringify(body));
      try {
        const { method, params } = JSON.parse(Buffer.concat(chunks).toString("utf8")) as { method: string; params?: object };
        engine.call(method, params).then(
          (result) => send(200, { result }),
          (error: unknown) =>
            error instanceof EngineRpcError
              ? send(200, { error: { code: error.code, message: error.message } })
              : send(500, { error: { code: -32000, message: String(error) } }),
        );
      } catch {
        send(400, { error: { code: -32700, message: "invalid request body" } });
      }
    });
    return;
  }
  response.writeHead(404).end();
});

server.listen(port, "127.0.0.1", () => console.log(`ananas dev bridge on http://127.0.0.1:${port}`));
process.on("SIGINT", () => void engine.close().then(() => process.exit(0)));
