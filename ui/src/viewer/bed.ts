// The print bed: its outline (the printer's printable_area), its height and whether an object fits on it.

export type Point = readonly [number, number];

export interface Box {
  min: readonly [number, number, number];
  max: readonly [number, number, number];
}

export interface Bed {
  polygon: readonly Point[];
  min: Point;
  max: Point;
  center: Point;
  /** printable_height in mm. */
  height: number;
}

const FALLBACK: Point[] = [
  [0, 0],
  [220, 0],
  [220, 220],
  [0, 220],
];

/** Reads "0x0,350x0,350x350,0x350" (the serialized printable_area); a missing or broken value falls back to 220 x 220. */
export function parseBed(printableArea: string | undefined, printableHeight: string | undefined): Bed {
  let polygon: Point[] = [];
  if (printableArea) {
    for (const pair of printableArea.split(",")) {
      const [x, y] = pair.split("x").map(Number);
      if (x === undefined || y === undefined || !Number.isFinite(x) || !Number.isFinite(y)) {
        polygon = [];
        break;
      }
      polygon.push([x, y]);
    }
  }
  if (polygon.length < 3) polygon = FALLBACK;
  const xs = polygon.map((point) => point[0]);
  const ys = polygon.map((point) => point[1]);
  const min: Point = [Math.min(...xs), Math.min(...ys)];
  const max: Point = [Math.max(...xs), Math.max(...ys)];
  const height = Number(printableHeight);
  return {
    polygon,
    min,
    max,
    center: [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2],
    height: Number.isFinite(height) && height > 0 ? height : 250,
  };
}

/** Even-odd point in polygon; a point on the outline counts as inside. */
export function pointInPolygon(point: Point, polygon: readonly Point[]): boolean {
  const [x, y] = point;
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i, i += 1) {
    const [xi, yi] = polygon[i]!;
    const [xj, yj] = polygon[j]!;
    const cross = (xj - xi) * (y - yi) - (x - xi) * (yj - yi);
    const onSegment =
      Math.abs(cross) < 1e-9 && x >= Math.min(xi, xj) - 1e-9 && x <= Math.max(xi, xj) + 1e-9 && y >= Math.min(yi, yj) - 1e-9 && y <= Math.max(yi, yj) + 1e-9;
    if (onSegment) return true;
    if (yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

/** The object's footprint corners lie on the bed and it is not taller than the printer. */
export function fitsBed(box: Box, bed: Bed): boolean {
  const corners: Point[] = [
    [box.min[0], box.min[1]],
    [box.max[0], box.min[1]],
    [box.max[0], box.max[1]],
    [box.min[0], box.max[1]],
  ];
  return corners.every((corner) => pointInPolygon(corner, bed.polygon)) && box.max[2] <= bed.height + 1e-6;
}
