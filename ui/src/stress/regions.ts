// Flächenwahl im Viewport: ein Klick wählt die ebene Fläche unter dem Zeiger (alle zusammenhängenden Dreiecke
// mit fast gleicher Normale), so wie man es aus CAD-Programmen kennt. Die Engine bekommt Dreiecksindizes über
// alle Modellteile eines Objekts in der Reihenfolge von `model.load`.

import type { PartMesh } from "../viewer/objects.ts";

/** Dreiecke, die weniger als dieser Winkel (Grad) von der Normale des angeklickten abweichen, gehören zur Fläche. */
export const PLANAR_ANGLE_DEG = 8;

/** Index des ersten Dreiecks jedes Teils im Index über alle Teile. */
export function partOffsets(parts: readonly Pick<PartMesh, "indices">[]): number[] {
  const offsets: number[] = [];
  let total = 0;
  for (const part of parts) {
    offsets.push(total);
    total += part.indices.length / 3;
  }
  return offsets;
}

/** Der Index eines Dreiecks über alle Teile; null, wenn Teil oder Dreieck nicht existiert. */
export function globalTriangle(parts: readonly Pick<PartMesh, "indices">[], part: number, triangle: number): number | null {
  const mesh = parts[part];
  if (mesh === undefined || triangle < 0 || triangle >= mesh.indices.length / 3) return null;
  return partOffsets(parts)[part]! + triangle;
}

function normalOf(positions: Float32Array, indices: Uint32Array, triangle: number): [number, number, number] {
  const a = indices[triangle * 3]! * 3;
  const b = indices[triangle * 3 + 1]! * 3;
  const c = indices[triangle * 3 + 2]! * 3;
  const ux = positions[b]! - positions[a]!, uy = positions[b + 1]! - positions[a + 1]!, uz = positions[b + 2]! - positions[a + 2]!;
  const vx = positions[c]! - positions[a]!, vy = positions[c + 1]! - positions[a + 1]!, vz = positions[c + 2]! - positions[a + 2]!;
  const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  const length = Math.hypot(nx, ny, nz);
  return length > 0 ? [nx / length, ny / length, nz / length] : [0, 0, 0];
}

/**
 * Die zusammenhängende ebene Fläche, zu der `start` gehört, als Dreiecksindizes dieses Teils (aufsteigend).
 * Zusammenhang heißt: gemeinsame Kante, auch über doppelte Eckpunkte hinweg (die Lage zählt, nicht der Index).
 * Ein entartetes Startdreieck liefert nur sich selbst.
 */
export function planarRegion(positions: Float32Array, indices: Uint32Array, start: number, maxAngleDeg = PLANAR_ANGLE_DEG): number[] {
  const count = indices.length / 3;
  if (start < 0 || start >= count) return [];
  const reference = normalOf(positions, indices, start);
  if (reference[0] === 0 && reference[1] === 0 && reference[2] === 0) return [start];
  const cosLimit = Math.cos((maxAngleDeg * Math.PI) / 180);

  // Eckpunkte gleicher Lage bekommen dieselbe Nummer, damit auch aufgetrennte Netze zusammenhängen.
  const keyOf = new Map<string, number>();
  const vertexId = new Uint32Array(positions.length / 3);
  for (let v = 0; v < vertexId.length; v += 1) {
    const key = `${positions[v * 3]},${positions[v * 3 + 1]},${positions[v * 3 + 2]}`;
    let id = keyOf.get(key);
    if (id === undefined) {
      id = keyOf.size;
      keyOf.set(key, id);
    }
    vertexId[v] = id;
  }
  const edgeKey = (p: number, q: number): string => (p < q ? `${p}:${q}` : `${q}:${p}`);
  const trianglesOfEdge = new Map<string, number[]>();
  for (let t = 0; t < count; t += 1) {
    const ids = [vertexId[indices[t * 3]!]!, vertexId[indices[t * 3 + 1]!]!, vertexId[indices[t * 3 + 2]!]!];
    for (let k = 0; k < 3; k += 1) {
      const key = edgeKey(ids[k]!, ids[(k + 1) % 3]!);
      const list = trianglesOfEdge.get(key);
      if (list === undefined) trianglesOfEdge.set(key, [t]);
      else list.push(t);
    }
  }

  const seen = new Uint8Array(count);
  const region: number[] = [];
  const queue = [start];
  seen[start] = 1;
  while (queue.length > 0) {
    const t = queue.pop()!;
    region.push(t);
    const ids = [vertexId[indices[t * 3]!]!, vertexId[indices[t * 3 + 1]!]!, vertexId[indices[t * 3 + 2]!]!];
    for (let k = 0; k < 3; k += 1) {
      for (const next of trianglesOfEdge.get(edgeKey(ids[k]!, ids[(k + 1) % 3]!)) ?? []) {
        if (seen[next] === 1) continue;
        const n = normalOf(positions, indices, next);
        // An der Startnormale gemessen, nicht am Nachbarn: eine allmähliche Krümmung darf nicht durchrutschen.
        if (n[0] * reference[0] + n[1] * reference[1] + n[2] * reference[2] >= cosLimit) {
          seen[next] = 1;
          queue.push(next);
        }
      }
    }
  }
  return region.sort((a, b) => a - b);
}

/** Vereinigung ohne Doppelte, aufsteigend. */
export function mergeTriangles(a: readonly number[], b: readonly number[]): number[] {
  return [...new Set([...a, ...b])].sort((x, y) => x - y);
}

/** Entfernt die Dreiecke von `remove` aus `from`. */
export function subtractTriangles(from: readonly number[], remove: readonly number[]): number[] {
  const drop = new Set(remove);
  return from.filter((triangle) => !drop.has(triangle));
}
