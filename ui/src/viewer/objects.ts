// An object on the bed as the viewer and the session know it: its parts' meshes, its placement and the
// matrix that puts the mesh exactly where the engine puts it.

import { Euler, Matrix4, Vector3 } from "three";
import type { ModelLoadResult, ObjectJson, ObjectTransform, MeshStats } from "../engine/protocol.ts";
import type { Box } from "./bed.ts";
import { toFloat32, toUint32 } from "./decode.ts";

export interface PartMesh {
  name: string;
  positions: Float32Array;
  indices: Uint32Array;
  triangles: number;
}

export interface Transform {
  /** Centre of the transformed object's bounding box on the bed, mm. */
  position: [number, number];
  /** Degrees, applied as Rz * Ry * Rx. */
  rotation: [number, number, number];
  /** Factors, 1 = as loaded. */
  scale: [number, number, number];
}

export interface SceneObject {
  id: number;
  name: string;
  /** Mesh in the object's own coordinates (before the transform). */
  parts: PartMesh[];
  rawBox: Box;
  stats: MeshStats;
  transform: Transform;
  /** Bounding box after the transform, as the engine computed it. */
  box: Box;
  /** The instance offset the engine used; together with rotation and scale it is the object's matrix. */
  offset: [number, number, number];
}

export const IDENTITY_ROTATION: [number, number, number] = [0, 0, 0];
export const IDENTITY_SCALE: [number, number, number] = [1, 1, 1];

export function toEngineTransform(transform: Transform): ObjectTransform {
  return { position: [...transform.position], rotation: [...transform.rotation], scale: [...transform.scale] };
}

/** T(offset) * Rz * Ry * Rx * S: the same composition the slicer uses for a model instance. */
export function worldMatrix(transform: Transform, offset: readonly [number, number, number]): Matrix4 {
  const deg = Math.PI / 180;
  const rotation = new Matrix4().makeRotationFromEuler(
    new Euler(transform.rotation[0] * deg, transform.rotation[1] * deg, transform.rotation[2] * deg, "ZYX"),
  );
  return new Matrix4()
    .makeTranslation(offset[0], offset[1], offset[2])
    .multiply(rotation)
    .multiply(new Matrix4().makeScale(transform.scale[0], transform.scale[1], transform.scale[2]));
}

/** Row-major copy of a matrix (three stores column-major). */
export function rowMajor(matrix: Matrix4): number[] {
  const e = matrix.elements;
  return [e[0]!, e[4]!, e[8]!, e[12]!, e[1]!, e[5]!, e[9]!, e[13]!, e[2]!, e[6]!, e[10]!, e[14]!, e[3]!, e[7]!, e[11]!, e[15]!];
}

/** Exact bounding box of the vertices after the matrix. */
export function transformedBox(parts: readonly PartMesh[], matrix: Matrix4): Box {
  const min = new Vector3(Infinity, Infinity, Infinity);
  const max = new Vector3(-Infinity, -Infinity, -Infinity);
  const point = new Vector3();
  for (const part of parts) {
    for (let i = 0; i < part.positions.length; i += 3) {
      point.set(part.positions[i]!, part.positions[i + 1]!, part.positions[i + 2]!).applyMatrix4(matrix);
      min.min(point);
      max.max(point);
    }
  }
  return { min: [min.x, min.y, min.z], max: [max.x, max.y, max.z] };
}

export function sizeOf(box: Box): [number, number, number] {
  return [box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]];
}

function decodeObject(json: ObjectJson, id: number, transform: Transform): SceneObject {
  return {
    id,
    name: json.name,
    parts: json.parts.map((part) => ({
      name: part.name,
      positions: toFloat32(part.vertices),
      indices: toUint32(part.indices),
      triangles: part.triangles,
    })),
    rawBox: json.bbox,
    stats: json.stats,
    transform,
    box: json.placement?.bbox ?? json.bbox,
    offset: [0, 0, 0],
  };
}

/**
 * The objects of a loaded file. A file with one object puts it in the middle of the bed; a file with several
 * keeps their layout and moves the group as a whole to the middle of the bed. The engine's bounds then fix each
 * object's box and offset.
 */
export function objectsFromModel(model: ModelLoadResult, center: readonly [number, number]): SceneObject[] {
  const placed = model.objects.map((json) => ({
    json,
    position: json.placement?.position ?? [center[0], center[1]],
    rotation: json.placement?.rotation ?? [...IDENTITY_ROTATION],
    scale: json.placement?.scale ?? [...IDENTITY_SCALE],
    box: json.placement?.bbox ?? json.bbox,
  }));
  let shift: [number, number] = [0, 0];
  if (placed.length === 1) {
    shift = [center[0] - placed[0]!.position[0], center[1] - placed[0]!.position[1]];
  } else if (placed.length > 1) {
    const minX = Math.min(...placed.map((p) => p.box.min[0]));
    const maxX = Math.max(...placed.map((p) => p.box.max[0]));
    const minY = Math.min(...placed.map((p) => p.box.min[1]));
    const maxY = Math.max(...placed.map((p) => p.box.max[1]));
    shift = [center[0] - (minX + maxX) / 2, center[1] - (minY + maxY) / 2];
  }
  return placed.map((p, index) =>
    decodeObject(p.json, index, {
      position: [p.position[0] + shift[0], p.position[1] + shift[1]],
      rotation: [...p.rotation] as [number, number, number],
      scale: [...p.scale] as [number, number, number],
    }),
  );
}
