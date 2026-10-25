// Die Oberfläche des Voxelmodells als Dreiecksnetz, je Zelle in der Farbe ihres Wertes. Gezeichnet wird, was die
// Rechnung gesehen hat: die gestufte Oberfläche, nicht das glatte Netz. Mit `clipLayer` wird das Gitter oberhalb
// einer Schicht weggelassen; die Schnittfläche zeigt dann die Werte im Inneren.

import { Color } from "three";
import { binFor, type StressQuantity, type StressScale } from "./colors.ts";
import { OVER_COLOR, RAMP } from "../viewer/heatmap.ts";
import { cellIndex, type StressField } from "./field.ts";

export interface StressMesh {
  /** 4 Ecken je Fläche. */
  positions: Float32Array;
  colors: Float32Array;
  indices: Uint32Array;
  faces: number;
}

const FACE_NORMALS: readonly (readonly [number, number, number])[] = [
  [1, 0, 0],
  [-1, 0, 0],
  [0, 1, 0],
  [0, -1, 0],
  [0, 0, 1],
  [0, 0, -1],
];

/** Ecken (Einheitswürfel) je Fläche, gegen den Uhrzeigersinn von außen gesehen. */
const FACE_CORNERS: readonly (readonly (readonly [number, number, number])[])[] = [
  [[1, 0, 0], [1, 1, 0], [1, 1, 1], [1, 0, 1]],
  [[0, 0, 0], [0, 0, 1], [0, 1, 1], [0, 1, 0]],
  [[0, 1, 0], [0, 1, 1], [1, 1, 1], [1, 1, 0]],
  [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]],
  [[0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]],
  [[0, 0, 0], [0, 1, 0], [1, 1, 0], [1, 0, 0]],
];

const PALETTE = new Map<string, Color>();
function paletteColor(hex: string): Color {
  let color = PALETTE.get(hex);
  if (color === undefined) {
    color = new Color(hex);
    PALETTE.set(hex, color);
  }
  return color;
}

/** Der Wert einer Zelle in der gewählten Größe. */
export function cellValue(field: StressField, quantity: StressQuantity, index: number): number {
  if (quantity === "von-mises") return field.vonMises[index]!;
  if (quantity === "safety") return field.safety[index]!;
  return field.displacement[index]!;
}

/** `clipLayer`: höchste gezeichnete Gitterschicht (inklusive); null = alles. */
export function buildStressMesh(field: StressField, stress: StressScale, clipLayer: number | null): StressMesh {
  const [nx, ny, nz] = field.size;
  const top = clipLayer === null ? nz - 1 : Math.min(nz - 1, Math.max(-1, clipLayer));
  const solidAt = (x: number, y: number, z: number): boolean =>
    x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z <= top && field.solid[cellIndex(field, x, y, z)] === 1;

  // Erst zählen, damit die Arrays genau passen.
  let faces = 0;
  for (let z = 0; z <= top; z += 1) {
    for (let y = 0; y < ny; y += 1) {
      for (let x = 0; x < nx; x += 1) {
        if (field.solid[cellIndex(field, x, y, z)] !== 1) continue;
        for (const [dx, dy, dz] of FACE_NORMALS) if (!solidAt(x + dx, y + dy, z + dz)) faces += 1;
      }
    }
  }

  const positions = new Float32Array(faces * 12);
  const colors = new Float32Array(faces * 12);
  const indices = new Uint32Array(faces * 6);
  const h = field.voxelSize;
  let face = 0;
  for (let z = 0; z <= top; z += 1) {
    for (let y = 0; y < ny; y += 1) {
      for (let x = 0; x < nx; x += 1) {
        const index = cellIndex(field, x, y, z);
        if (field.solid[index] !== 1) continue;
        const bin = binFor(stress, cellValue(field, stress.quantity, index));
        const color = paletteColor(bin === -1 ? OVER_COLOR : RAMP[bin]!);
        for (let side = 0; side < 6; side += 1) {
          const [dx, dy, dz] = FACE_NORMALS[side]!;
          if (solidAt(x + dx, y + dy, z + dz)) continue;
          const corners = FACE_CORNERS[side]!;
          for (let k = 0; k < 4; k += 1) {
            const corner = corners[k]!;
            const at = (face * 4 + k) * 3;
            positions[at] = field.origin[0] + (x + corner[0]) * h;
            positions[at + 1] = field.origin[1] + (y + corner[1]) * h;
            positions[at + 2] = field.origin[2] + (z + corner[2]) * h;
            colors[at] = color.r;
            colors[at + 1] = color.g;
            colors[at + 2] = color.b;
          }
          const base = face * 4;
          indices.set([base, base + 1, base + 2, base, base + 2, base + 3], face * 6);
          face += 1;
        }
      }
    }
  }
  return { positions, colors, indices, faces };
}
