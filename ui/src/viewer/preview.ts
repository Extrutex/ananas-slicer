// G-code toolpaths as the viewer draws them: segments grouped by layer, split into extrusions and travel moves.

import type { PreviewChunkJson, PreviewSummary } from "../engine/protocol.ts";
import { toFloat32, toUint16, toUint8 } from "./decode.ts";
import type { Scale } from "./heatmap.ts";

export interface SegmentSet {
  count: number;
  /** x0 y0 z0 x1 y1 z1 per segment. */
  positions: Float32Array;
  speed: Float32Array;
  flow: Float32Array;
  accel: Float32Array;
  role: Uint8Array;
  /** Index into the summary's layer list. */
  layer: Uint16Array;
  /** First segment of every layer, plus the total at the end (length = layers + 1). */
  layerStart: Uint32Array;
}

export interface PreviewData {
  summary: PreviewSummary;
  extrusion: SegmentSet;
  travel: SegmentSet;
}

interface Decoded {
  count: number;
  positions: Float32Array;
  speed: Float32Array;
  flow: Float32Array;
  accel: Float32Array;
  role: Uint8Array;
  kind: Uint8Array;
  layer: Uint16Array;
}

function decode(chunk: PreviewChunkJson): Decoded | null {
  if (chunk.count === 0) return null;
  return {
    count: chunk.count,
    positions: toFloat32(chunk.positions!),
    speed: toFloat32(chunk.speed!),
    flow: toFloat32(chunk.flow!),
    accel: toFloat32(chunk.accel!),
    role: toUint8(chunk.role!),
    kind: toUint8(chunk.kind!),
    layer: toUint16(chunk.layer!),
  };
}

function build(parts: Decoded[], kind: number, layerCount: number): SegmentSet {
  let count = 0;
  for (const part of parts) for (let i = 0; i < part.count; i += 1) if (part.kind[i] === kind) count += 1;

  const set: SegmentSet = {
    count,
    positions: new Float32Array(count * 6),
    speed: new Float32Array(count),
    flow: new Float32Array(count),
    accel: new Float32Array(count),
    role: new Uint8Array(count),
    layer: new Uint16Array(count),
    layerStart: new Uint32Array(layerCount + 1),
  };
  let at = 0;
  for (const part of parts) {
    for (let i = 0; i < part.count; i += 1) {
      if (part.kind[i] !== kind) continue;
      set.positions.set(part.positions.subarray(i * 6, i * 6 + 6), at * 6);
      set.speed[at] = part.speed[i]!;
      set.flow[at] = part.flow[i]!;
      set.accel[at] = part.accel[i]!;
      set.role[at] = part.role[i]!;
      set.layer[at] = part.layer[i]!;
      at += 1;
    }
  }
  // Chunks come in layer order, so the segments of a layer are contiguous: one pass gives each layer's start.
  const starts = set.layerStart;
  starts.fill(count);
  for (let i = count - 1; i >= 0; i -= 1) starts[set.layer[i]!] = i;
  for (let layer = layerCount - 1; layer >= 0; layer -= 1) if (starts[layer]! > starts[layer + 1]!) starts[layer] = starts[layer + 1]!;
  return set;
}

/** Joins the chunks of one file (layer order) into the arrays the viewer uploads. */
export function mergeChunks(summary: PreviewSummary, chunks: readonly PreviewChunkJson[]): PreviewData {
  const decoded = chunks.map(decode).filter((part): part is Decoded => part !== null);
  const layerCount = summary.layers.length;
  return { summary, extrusion: build(decoded, 0, layerCount), travel: build(decoded, 1, layerCount) };
}

export interface LimitValues {
  max_velocity: number;
  max_accel: number;
}

/** The heatmap scale of a quantity: the file's own range, and the machine limit above which a value is out of range. */
export function previewScale(summary: PreviewSummary, quantity: "speed" | "flow" | "accel", limits: LimitValues | null): Scale {
  const range = summary[quantity];
  const limit = quantity === "speed" ? (limits?.max_velocity ?? null) : quantity === "accel" ? (limits?.max_accel ?? null) : null;
  return { min: range.min, max: range.max, limit };
}
