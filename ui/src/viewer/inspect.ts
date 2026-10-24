// The inspection view of a model: which faces need support, which lie on the bed. A pure function of the mesh
// and the object's rotation, so it can follow every turn of the object.

export type FaceClass = "ok" | "caution" | "overhang" | "contact";

export const FACE_COLORS: Readonly<Record<FaceClass, string>> = {
  ok: "#8d9599",
  caution: "#ffc56a",
  overhang: "#ff6f5e",
  contact: "#4db9ff",
};

export interface InspectResult {
  /** Class of every triangle. */
  classes: Uint8Array;
  /** Share of the surface area per class (0..1). */
  share: Readonly<Record<FaceClass, number>>;
}

export const CLASS_ORDER: readonly FaceClass[] = ["ok", "caution", "overhang", "contact"];

/** How many degrees above the threshold a face is still marked as a caution. */
const CAUTION_BAND_DEG = 10;
/** A face counts as lying on the bed when it points straight down and its lowest point is this close to z = 0. */
const CONTACT_HEIGHT = 0.05;
const CONTACT_FLATNESS = 0.999;

/**
 * Classifies every triangle of an object as it stands on the bed.
 *
 * `worldMatrix` is the object's 4x4 transform, row-major; triangles are measured in world space, so areas and
 * normals are right under any rotation and scale. A face is an overhang when it points down and its slope
 * measured from the horizontal (0 deg = a ceiling, 90 deg = a wall) is below `thresholdDeg`, the support
 * threshold of the profile; the band above it is a caution, and a flat face on the bed is bed contact.
 */
export function classifyFaces(positions: Float32Array, indices: Uint32Array, worldMatrix: readonly number[], thresholdDeg: number): InspectResult {
  const count = indices.length / 3;
  const classes = new Uint8Array(count);
  const area: Record<FaceClass, number> = { ok: 0, caution: 0, overhang: 0, contact: 0 };
  let total = 0;
  const m = worldMatrix;
  const world = new Float64Array(9);

  for (let t = 0; t < count; t += 1) {
    for (let k = 0; k < 3; k += 1) {
      const vertex = indices[t * 3 + k]! * 3;
      const x = positions[vertex]!, y = positions[vertex + 1]!, z = positions[vertex + 2]!;
      world[k * 3] = m[0]! * x + m[1]! * y + m[2]! * z + m[3]!;
      world[k * 3 + 1] = m[4]! * x + m[5]! * y + m[6]! * z + m[7]!;
      world[k * 3 + 2] = m[8]! * x + m[9]! * y + m[10]! * z + m[11]!;
    }
    const ux = world[3]! - world[0]!, uy = world[4]! - world[1]!, uz = world[5]! - world[2]!;
    const vx = world[6]! - world[0]!, vy = world[7]! - world[1]!, vz = world[8]! - world[2]!;
    const nx = uy * vz - uz * vy;
    const ny = uz * vx - ux * vz;
    const nz = ux * vy - uy * vx;
    const doubleArea = Math.hypot(nx, ny, nz);
    if (doubleArea < 1e-12) {
      classes[t] = CLASS_ORDER.indexOf("ok");
      continue;
    }
    const down = -nz / doubleArea; // 1 = a ceiling, 0 = a wall, < 0 faces up

    let faceClass: FaceClass = "ok";
    if (down > 0) {
      const slope = (Math.acos(Math.min(1, down)) * 180) / Math.PI;
      const lowest = Math.min(world[2]!, world[5]!, world[8]!);
      if (down >= CONTACT_FLATNESS && lowest < CONTACT_HEIGHT) faceClass = "contact";
      else if (slope < thresholdDeg) faceClass = "overhang";
      else if (slope < thresholdDeg + CAUTION_BAND_DEG) faceClass = "caution";
    }
    classes[t] = CLASS_ORDER.indexOf(faceClass);
    const weight = doubleArea / 2;
    area[faceClass] += weight;
    total += weight;
  }

  const share = { ok: 0, caution: 0, overhang: 0, contact: 0 } as Record<FaceClass, number>;
  for (const key of CLASS_ORDER) share[key] = total > 0 ? area[key] / total : 0;
  return { classes, share };
}
