// Das Ergebnisfeld einer Kraftverteilung: die Zellen des Gitters als Typed Arrays, aus den Blöcken von
// `analyze.chunk` zusammengesetzt (x am schnellsten, dann y, dann z).

import type { StressChunkJson, StressSummary } from "../engine/protocol.ts";
import { toFloat32, toUint8 } from "../viewer/decode.ts";

export interface StressField {
  size: [number, number, number];
  voxelSize: number;
  /** Ecke des Gitters im Bettkoordinatensystem, mm. */
  origin: [number, number, number];
  /** 1 = Zelle liegt im Körper. */
  solid: Uint8Array;
  /** MPa */
  vonMises: Float32Array;
  /** Sicherheitsfaktor, nach oben bei `safetyCap` begrenzt; 0 außerhalb des Körpers. */
  safety: Float32Array;
  /** mm */
  displacement: Float32Array;
}

/** Pro Anfrage höchstens so viele Zellen: die Antwort ist Base64 in JSON (die Engine erlaubt 8 MB je Block, 335 544 Zellen; setup.ts prüft, dass eine Schicht hineinpasst). */
export const CHUNK_CELLS = 300_000;

/** Die z-Bereiche (von, bis, inklusive), in denen das Gitter abgeholt wird. */
export function planChunks(size: readonly [number, number, number]): { from: number; to: number }[] {
  const layer = size[0] * size[1];
  if (layer <= 0 || size[2] <= 0) return [];
  const layers = Math.max(1, Math.floor(CHUNK_CELLS / layer));
  const chunks: { from: number; to: number }[] = [];
  for (let from = 0; from < size[2]; from += layers) chunks.push({ from, to: Math.min(from + layers, size[2]) - 1 });
  return chunks;
}

export function allocateField(summary: StressSummary): StressField {
  const size = summary.grid.size;
  const cells = size[0] * size[1] * size[2];
  return {
    size: [size[0], size[1], size[2]],
    voxelSize: summary.grid.voxel_size,
    origin: [...summary.grid.origin],
    solid: new Uint8Array(cells),
    vonMises: new Float32Array(cells),
    safety: new Float32Array(cells),
    displacement: new Float32Array(cells),
  };
}

/** Schreibt einen Block an seine Stelle. Wirft, wenn Länge oder Bereich nicht zum Gitter passen. */
export function applyChunk(field: StressField, chunk: StressChunkJson): void {
  const layer = field.size[0] * field.size[1];
  const expected = (chunk.z_to - chunk.z_from + 1) * layer;
  if (chunk.z_from < 0 || chunk.z_to >= field.size[2] || chunk.count !== expected) {
    throw new Error(`Ungültiger Ergebnisblock: Schichten ${chunk.z_from}..${chunk.z_to}, ${chunk.count} Zellen.`);
  }
  const solid = toUint8(chunk.solid);
  const vonMises = toFloat32(chunk.von_mises);
  const safety = toFloat32(chunk.safety);
  const displacement = toFloat32(chunk.displacement);
  if (solid.length !== expected || vonMises.length !== expected || safety.length !== expected || displacement.length !== expected) {
    throw new Error("Ungültiger Ergebnisblock: die Feldlängen passen nicht zur Zellzahl.");
  }
  const at = chunk.z_from * layer;
  field.solid.set(solid, at);
  field.vonMises.set(vonMises, at);
  field.safety.set(safety, at);
  field.displacement.set(displacement, at);
}

export const cellIndex = (field: StressField, x: number, y: number, z: number): number => (z * field.size[1] + y) * field.size[0] + x;

/** Die Zelle, in der ein Punkt des Bettkoordinatensystems liegt, oder null außerhalb des Gitters. */
export function cellAt(field: StressField, point: readonly [number, number, number]): number | null {
  const x = Math.floor((point[0] - field.origin[0]) / field.voxelSize);
  const y = Math.floor((point[1] - field.origin[1]) / field.voxelSize);
  const z = Math.floor((point[2] - field.origin[2]) / field.voxelSize);
  if (x < 0 || y < 0 || z < 0 || x >= field.size[0] || y >= field.size[1] || z >= field.size[2]) return null;
  return cellIndex(field, x, y, z);
}
