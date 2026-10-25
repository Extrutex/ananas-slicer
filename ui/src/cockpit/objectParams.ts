// The selected object's placement as cockpit rows: position, rotation, scale and the resulting size.
// The ids are "object.<field>.<axis>"; the session maps an edit of one back onto the object's transform.

import type { CockpitGroup, CockpitParam } from "./model.ts";
import { sizeOf, type SceneObject } from "../viewer/objects.ts";

export const OBJECT_PREFIX = "object.";

export type TransformField = "position" | "rotation" | "scale";
export type Axis = "x" | "y" | "z";

const AXES: readonly Axis[] = ["x", "y", "z"];
export const axisIndex = (axis: Axis): number => AXES.indexOf(axis);

export function parseObjectId(id: string): { field: TransformField; axis: Axis } | null {
  const match = /^object\.(position|rotation|scale)\.([xyz])$/.exec(id);
  return match ? { field: match[1] as TransformField, axis: match[2] as Axis } : null;
}

function row(id: string, label: string, unit: string, value: number, step: number, decimals: number, min: number, max: number, readOnly = false): CockpitParam {
  return { id, label, unit, value, step, decimals, min, max, limit: null, readOnly };
}

/** The "Object" group for one object; scale is shown in percent. */
export function objectGroup(object: SceneObject): CockpitGroup {
  const { position, rotation, scale } = object.transform;
  const size = sizeOf(object.box);
  const params: CockpitParam[] = [
    row("object.position.x", "Position X", "mm", position[0], 1, 2, -5000, 5000),
    row("object.position.y", "Position Y", "mm", position[1], 1, 2, -5000, 5000),
    row("object.rotation.x", "Rotation X", "°", rotation[0], 1, 1, -360, 360),
    row("object.rotation.y", "Rotation Y", "°", rotation[1], 1, 1, -360, 360),
    row("object.rotation.z", "Rotation Z", "°", rotation[2], 1, 1, -360, 360),
    row("object.scale.x", "Scale X", "%", scale[0] * 100, 1, 1, 0.1, 1000),
    row("object.scale.y", "Scale Y", "%", scale[1] * 100, 1, 1, 0.1, 1000),
    row("object.scale.z", "Scale Z", "%", scale[2] * 100, 1, 1, 0.1, 1000),
    row("object.size.x", "Size X", "mm", size[0], 1, 2, 0, 1e6, true),
    row("object.size.y", "Size Y", "mm", size[1], 1, 2, 0, 1e6, true),
    row("object.size.z", "Size Z", "mm", size[2], 1, 2, 0, 1e6, true),
  ];
  return { id: "object", title: object.name || "Object", params };
}
