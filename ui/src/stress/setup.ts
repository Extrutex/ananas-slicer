// Was der Benutzer für eine Kraftverteilung festlegt (Werkstoff, Voxelgröße, Lagerflächen, Lasten) und wie
// daraus die Anfrage an die Engine wird. Reine Logik ohne Oberfläche: die Prüfregeln stehen hier, nicht in der
// Komponente, und die Engine bleibt die letzte Instanz.

import type { ObjectTransform, StressLoadJson, StressRequest, StressSupportJson } from "../engine/protocol.ts";
import type { Box } from "../viewer/bed.ts";
import { materialProblems, materialToJson, type StressMaterial } from "./materials.ts";

export type RegionKind = "support" | "load";
export type Vec3 = [number, number, number];

/** Eine Lagerfläche oder eine Lastfläche: Dreiecke aus dem Viewport und/oder eine Box im Bettkoordinatensystem. */
export interface StressRegion {
  id: number;
  kind: RegionKind;
  /** Indizes über die Dreiecke aller Modellteile des Objekts, in der Reihenfolge von model.load. */
  triangles: number[];
  box: Box | null;
  /** Nur Lager: welche Verschiebungen festgehalten werden. */
  axes: [boolean, boolean, boolean];
  /** Nur Last: Kraft in N, Resultierende auf der Fläche. */
  force: Vec3;
}

export interface StressSetup {
  material: StressMaterial;
  /** Kantenlänge der Voxel in mm. */
  voxelSize: number;
  regions: StressRegion[];
  /** Obergrenze der Rechenzeit in s (Wanduhr der ganzen Analyse); ohne Angabe gilt die Vorgabe der Engine. */
  maxSeconds?: number;
}

/** Die Engine lehnt mehr als 5 Millionen Gitterzellen ab (docs/HLSD/engine.md). */
export const MAX_CELLS = 5_000_000;
/** Ab dieser Zellzahl dauert die Rechnung und die Darstellung spürbar. */
export const HEAVY_CELLS = 2_000_000;
/** Vorgabe der Engine für die Rechenzeit (max_seconds), danach bricht sie mit einer Fehlermeldung ab. */
export const DEFAULT_MAX_SECONDS = 600;
/** Ein Ergebnisblock der Engine fasst höchstens 8 MB bei 25 Byte je Zelle; eine ganze Gitterschicht muss hineinpassen. */
export const CHUNK_MAX_BYTES = 8 * 1024 * 1024;
export const CHUNK_BYTES_PER_CELL = 25;
/** Die Engine nimmt höchstens so viele Lager und so viele Lasten an und Lasten bis zu dieser Größe in N je Richtung. */
export const MAX_REGIONS = 64;
export const MAX_FORCE = 1e12;

export function emptyRegion(id: number, kind: RegionKind): StressRegion {
  return { id, kind, triangles: [], box: null, axes: [true, true, true], force: [0, 0, 0] };
}

export function hasPlace(region: StressRegion): boolean {
  return region.triangles.length > 0 || region.box !== null;
}

/** Die Zellen des Gitters, das die Engine über das Objekt legt (Obergrenze: eine Zelle Rand je Seite). */
export function estimateCells(box: Box, voxelSize: number): number {
  if (!Number.isFinite(voxelSize) || voxelSize <= 0) return 0;
  let cells = 1;
  for (let axis = 0; axis < 3; axis += 1) cells *= Math.ceil((box.max[axis]! - box.min[axis]!) / voxelSize) + 1;
  return cells;
}

/** Die Zellen einer Gitterschicht (x mal y), wie die Engine sie zählt (Obergrenze: eine Zelle Rand je Seite). */
export function estimateLayerCells(box: Box, voxelSize: number): number {
  if (!Number.isFinite(voxelSize) || voxelSize <= 0) return 0;
  return (Math.ceil((box.max[0] - box.min[0]) / voxelSize) + 1) * (Math.ceil((box.max[1] - box.min[1]) / voxelSize) + 1);
}

/** Wie viele Zellen die dünnste Abmessung des Objekts bekommt. */
export function cellsAcrossThinnest(box: Box, voxelSize: number): number {
  const thinnest = Math.min(box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]);
  return voxelSize > 0 ? thinnest / voxelSize : 0;
}

export interface SetupCheck {
  /** Verhindern den Start. */
  errors: string[];
  /** Erlauben den Start, mindern aber die Aussagekraft. */
  hints: string[];
  cells: number;
}

export function checkSetup(setup: StressSetup, objectBox: Box | null): SetupCheck {
  const errors = [...materialProblems(setup.material)];
  const hints: string[] = [];
  if (!Number.isFinite(setup.voxelSize) || setup.voxelSize <= 0) errors.push("Die Voxelgröße muss größer als 0 sein.");
  const supports = setup.regions.filter((region) => region.kind === "support");
  const loads = setup.regions.filter((region) => region.kind === "load");
  if (!supports.some(hasPlace)) errors.push("Mindestens eine Lagerfläche fehlt: ohne festgehaltene Fläche kann sich das Teil frei bewegen.");
  if (!loads.some(hasPlace)) errors.push("Mindestens eine Lastfläche fehlt.");
  for (const region of setup.regions) {
    const name = regionName(region, setup.regions);
    if (!hasPlace(region)) errors.push(`${name}: keine Fläche und keine Box gewählt.`);
    if (region.kind === "support" && !region.axes.some(Boolean)) errors.push(`${name}: keine Achse festgehalten.`);
    if (region.kind === "load" && !region.force.every(Number.isFinite)) errors.push(`${name}: die Kraft ist keine Zahl.`);
    if (region.kind === "load" && region.force.every((component) => component === 0)) errors.push(`${name}: die Kraft ist 0 N.`);
    if (region.kind === "load" && region.force.some((component) => Math.abs(component) > MAX_FORCE)) {
      errors.push(`${name}: die Kraft ist größer als ${formatCount(MAX_FORCE)} N.`);
    }
    if (region.box !== null && region.box.min.some((value, axis) => value > region.box!.max[axis]!)) {
      errors.push(`${name}: die Box hat ein Minimum über dem Maximum.`);
    }
  }
  if (supports.length > MAX_REGIONS || loads.length > MAX_REGIONS) errors.push(`Höchstens ${MAX_REGIONS} Lager und ${MAX_REGIONS} Lasten.`);
  if (setup.maxSeconds !== undefined && !(setup.maxSeconds > 0 && Number.isFinite(setup.maxSeconds))) errors.push("Die Rechenzeit muss größer als 0 s sein.");
  let cells = 0;
  if (objectBox !== null && setup.voxelSize > 0) {
    cells = estimateCells(objectBox, setup.voxelSize);
    if (cells > MAX_CELLS) {
      errors.push(`Das Gitter hätte etwa ${formatCount(cells)} Zellen, die Engine rechnet höchstens ${formatCount(MAX_CELLS)}. Voxel vergrößern.`);
    } else if (cells > HEAVY_CELLS) {
      const limit = setup.maxSeconds ?? DEFAULT_MAX_SECONDS;
      hints.push(`Etwa ${formatCount(cells)} Zellen: Rechnung und Darstellung dauern spürbar. Die Engine bricht nach ${formatCount(limit)} s Rechenzeit ab (Vorbereitung und Lösen zusammen).`);
    }
    const layer = estimateLayerCells(objectBox, setup.voxelSize);
    if (layer * CHUNK_BYTES_PER_CELL > CHUNK_MAX_BYTES) {
      errors.push(
        `Eine Gitterschicht hätte etwa ${formatCount(layer)} Zellen, die Engine liefert höchstens ${formatCount(Math.floor(CHUNK_MAX_BYTES / CHUNK_BYTES_PER_CELL))} je Schicht (ein Ergebnisblock fasst 8 MB). Voxel vergrößern.`,
      );
    }
    const across = cellsAcrossThinnest(objectBox, setup.voxelSize);
    if (across < 4) {
      hints.push(`Die dünnste Abmessung bekommt nur etwa ${across.toFixed(1)} Zellen: bei Biegung rechnet das Modell zu steif. Voxel verkleinern.`);
    }
  }
  return { errors, hints, cells };
}

const formatCount = (n: number): string => Math.round(n).toLocaleString("de-DE");

export function regionName(region: StressRegion, all: readonly StressRegion[]): string {
  const same = all.filter((candidate) => candidate.kind === region.kind);
  const position = same.findIndex((candidate) => candidate.id === region.id) + 1;
  return `${region.kind === "support" ? "Lager" : "Last"} ${position}`;
}

function selectionOf(region: StressRegion): { triangles?: number[]; box?: { min: Vec3; max: Vec3 } } {
  return {
    ...(region.triangles.length > 0 ? { triangles: [...region.triangles] } : {}),
    ...(region.box !== null ? { box: { min: [...region.box.min] as Vec3, max: [...region.box.max] as Vec3 } } : {}),
  };
}

/** Die Anfrage `analyze.stress`. Wirft, wenn die Prüfung Fehler findet, damit nie etwas Halbes gesendet wird. */
export function buildStressRequest(model: string, objectIndex: number, transform: ObjectTransform, setup: StressSetup): StressRequest {
  const check = checkSetup(setup, null);
  if (check.errors.length > 0) throw new Error(check.errors[0]);
  const supports: StressSupportJson[] = setup.regions
    .filter((region) => region.kind === "support" && hasPlace(region))
    .map((region) => ({ ...selectionOf(region), axes: [...region.axes] as [boolean, boolean, boolean] }));
  const loads: StressLoadJson[] = setup.regions
    .filter((region) => region.kind === "load" && hasPlace(region))
    .map((region) => ({ ...selectionOf(region), force: [...region.force] as Vec3 }));
  return {
    model,
    object: objectIndex,
    transform,
    material: materialToJson(setup.material),
    voxel_size: setup.voxelSize,
    supports,
    loads,
    ...(setup.maxSeconds !== undefined ? { max_seconds: setup.maxSeconds } : {}),
  };
}
