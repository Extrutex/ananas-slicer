// Starts the real ananas-engine for the integration tests. Paths come from the environment and default to the
// build tree of this worktree.

import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { NodeEngineProcess } from "../src/engine/nodeProcess.ts";

const repo = resolve(import.meta.dirname, "../..");

export const ENGINE_BIN =
  process.env.ANANAS_ENGINE_BIN ?? join(repo, "build/mini/src/engine/Release/ananas-engine.app/Contents/MacOS/ananas-engine");
export const PROFILES = process.env.ANANAS_PROFILES_DIR ?? join(repo, "resources/profiles");
export const MODEL = join(repo, "tests/data/20mm_cube.obj");
export const PRINTER_CFG = join(repo, "tests/virtual-printer/printer.cfg");

export function startEngine(): NodeEngineProcess {
  return new NodeEngineProcess({
    binary: ENGINE_BIN,
    profiles: PROFILES,
    vendor: "Voron",
    defaults: { model: MODEL, outputDir: mkdtempSync(join(tmpdir(), "ananas-ui-")) },
  });
}
