// Die reine Logik der Kraftverteilung: Werkstoffe, Einrichtung, Ergebnisfeld, Farben, Netz und Flächenwahl.

import { describe, expect, it } from "vitest";
import type { StressSummary } from "../src/engine/protocol.ts";
import { SAFETY_TOP, binFor, colorFor, formatValue, stressLegend, stressScale } from "../src/stress/colors.ts";
import { CHUNK_CELLS, allocateField, applyChunk, cellAt, cellIndex, planChunks } from "../src/stress/field.ts";
import { CUSTOM_MATERIAL_ID, MATERIAL_PRESETS, materialProblems, presetIdOf, type StressMaterial } from "../src/stress/materials.ts";
import { buildStressMesh } from "../src/stress/mesh.ts";
import { globalTriangle, mergeTriangles, partOffsets, planarRegion, subtractTriangles } from "../src/stress/regions.ts";
import { MAX_CELLS, buildStressRequest, checkSetup, emptyRegion, estimateCells, type StressSetup } from "../src/stress/setup.ts";
import { OVER_COLOR, RAMP } from "../src/viewer/heatmap.ts";

const PLA = MATERIAL_PRESETS[0]!;
const material = (): StressMaterial => ({ youngsModulus: PLA.youngsModulus, yieldStrength: PLA.yieldStrength, poissonRatio: PLA.poissonRatio, zFactor: PLA.zFactor });
const BOX = { min: [0, 0, 0], max: [20, 20, 20] } as const;

function validSetup(): StressSetup {
  const support = { ...emptyRegion(1, "support"), box: { min: [0, 0, 0], max: [20, 20, 0] } as const };
  const load = { ...emptyRegion(2, "load"), triangles: [4, 5], force: [0, 0, -50] as [number, number, number] };
  return { material: material(), voxelSize: 2, regions: [support, load] };
}

describe("Werkstoffe", () => {
  it("bietet PLA, PETG, ABS, ASA und PA-CF an", () => {
    expect(MATERIAL_PRESETS.map((preset) => preset.label)).toEqual(["PLA", "PETG", "ABS", "ASA", "PA-CF"]);
    for (const preset of MATERIAL_PRESETS) expect(materialProblems(preset)).toEqual([]);
  });

  it("erkennt eine Voreinstellung an ihren Werten und alles andere als eigene Werte", () => {
    expect(presetIdOf(material())).toBe("pla");
    expect(presetIdOf({ ...material(), yieldStrength: 51 })).toBe(CUSTOM_MATERIAL_ID);
  });

  it("verwirft unmögliche Werte, ohne sie zu klemmen", () => {
    expect(materialProblems({ ...material(), youngsModulus: 0 })).toHaveLength(1);
    expect(materialProblems({ ...material(), yieldStrength: Number.NaN })).toHaveLength(1);
    expect(materialProblems({ ...material(), poissonRatio: 0.5 })).toHaveLength(1);
    expect(materialProblems({ ...material(), zFactor: 0.04 })).toHaveLength(1);
    expect(materialProblems({ ...material(), zFactor: 1.2 })).toHaveLength(1);
    expect(materialProblems({ ...material(), zFactor: 0.05 })).toEqual([]);
  });
});

describe("Einrichtung", () => {
  it("baut die Anfrage der Engine mit Material, Lager, Last und Platzierung", () => {
    const request = buildStressRequest("/m.stl", 1, { position: [10, 20] }, validSetup());
    expect(request).toEqual({
      model: "/m.stl",
      object: 1,
      transform: { position: [10, 20] },
      material: { youngs_modulus: 3000, yield_strength: 45, poisson_ratio: 0.35, z_factor: 0.6 },
      voxel_size: 2,
      supports: [{ box: { min: [0, 0, 0], max: [20, 20, 0] }, axes: [true, true, true] }],
      loads: [{ triangles: [4, 5], force: [0, 0, -50] }],
    });
  });

  it("verlangt Lager, Last, eine Fläche je Eintrag und eine Kraft", () => {
    const none = checkSetup({ ...validSetup(), regions: [] }, null);
    expect(none.errors.join(" ")).toMatch(/Lagerfläche/);
    expect(none.errors.join(" ")).toMatch(/Lastfläche/);

    const empty = validSetup();
    empty.regions.push(emptyRegion(3, "load"));
    expect(checkSetup(empty, null).errors).toContain("Last 2: keine Fläche und keine Box gewählt.");

    const zero = validSetup();
    zero.regions[1]!.force = [0, 0, 0];
    expect(checkSetup(zero, null).errors).toContain("Last 1: die Kraft ist 0 N.");

    const free = validSetup();
    free.regions[0]!.axes = [false, false, false];
    expect(checkSetup(free, null).errors).toContain("Lager 1: keine Achse festgehalten.");

    const inverted = validSetup();
    inverted.regions[0]!.box = { min: [5, 0, 0], max: [1, 0, 0] };
    expect(checkSetup(inverted, null).errors.join(" ")).toMatch(/Minimum über dem Maximum/);
  });

  it("wirft statt eine halbe Anfrage zu senden", () => {
    const setup = validSetup();
    setup.voxelSize = 0;
    expect(() => buildStressRequest("/m.stl", 0, {}, setup)).toThrow(/Voxelgröße/);
  });

  it("schätzt die Zellen und warnt vor zu groben und zu feinen Gittern", () => {
    expect(estimateCells(BOX, 2)).toBe(11 ** 3);
    expect(estimateCells(BOX, 0)).toBe(0);
    const fine = checkSetup({ ...validSetup(), voxelSize: 0.05 }, BOX);
    expect(fine.cells).toBeGreaterThan(MAX_CELLS);
    expect(fine.errors.join(" ")).toMatch(/Voxel vergrößern/);
    const coarse = checkSetup({ ...validSetup(), voxelSize: 10 }, BOX);
    expect(coarse.errors).toEqual([]);
    expect(coarse.hints.join(" ")).toMatch(/zu steif/);
    const good = checkSetup(validSetup(), BOX);
    expect(good.errors).toEqual([]);
    expect(good.hints).toEqual([]);
  });
});

function summary(size: [number, number, number], origin: [number, number, number] = [0, 0, 0]): StressSummary {
  return {
    id: 1,
    grid: { size, voxel_size: 2, origin },
    solid_cells: size[0] * size[1] * size[2],
    nodes: 0,
    fixed_dofs: 0,
    solver: { iterations: 1, residual: 0, converged: true },
    max_von_mises: { value: 100, at: [0, 0, 0] },
    min_safety: { value: 0.5, at: [0, 0, 0] },
    max_displacement: { value: 0.2, at: [0, 0, 0] },
    applied_force: [0, 0, -1],
    reaction: [0, 0, 1],
    safety_cap: 1000,
  };
}

const b64 = (bytes: Uint8Array): string => Buffer.from(bytes).toString("base64");
const f32 = (values: number[]): string => b64(new Uint8Array(new Float32Array(values).buffer));

function chunk(zFrom: number, zTo: number, layer: number, fill: (cell: number) => { solid: number; vm: number; sf: number; d: number }) {
  const count = (zTo - zFrom + 1) * layer;
  const cells = Array.from({ length: count }, (_, i) => fill(zFrom * layer + i));
  return {
    id: 1,
    z_from: zFrom,
    z_to: zTo,
    count,
    solid: b64(Uint8Array.from(cells.map((c) => c.solid))),
    von_mises: f32(cells.map((c) => c.vm)),
    safety: f32(cells.map((c) => c.sf)),
    displacement: f32(cells.map((c) => c.d)),
    displacement_vec: f32(cells.flatMap((c) => [0, 0, c.d])),
  };
}

describe("Ergebnisfeld", () => {
  it("plant Blöcke, die das Gitter lückenlos und in Größen unter der Grenze abdecken", () => {
    expect(planChunks([10, 10, 10])).toEqual([{ from: 0, to: 9 }]);
    const big = planChunks([1000, 1000, 5]);
    expect(big).toEqual([{ from: 0, to: 0 }, { from: 1, to: 1 }, { from: 2, to: 2 }, { from: 3, to: 3 }, { from: 4, to: 4 }]);
    const mixed = planChunks([100, 100, 250]);
    expect(mixed.every((c) => (c.to - c.from + 1) * 100 * 100 <= CHUNK_CELLS)).toBe(true);
    expect(mixed[0]!.from).toBe(0);
    expect(mixed[mixed.length - 1]!.to).toBe(249);
    expect(planChunks([0, 3, 3])).toEqual([]);
  });

  it("setzt Blöcke an ihre Stelle und findet Zellen im Bettkoordinatensystem", () => {
    const field = allocateField(summary([2, 2, 3], [10, 20, 0]));
    const fill = (cell: number) => ({ solid: cell % 2, vm: cell, sf: 1 + cell, d: cell / 10 });
    applyChunk(field, chunk(0, 1, 4, fill));
    applyChunk(field, chunk(2, 2, 4, fill));
    expect(field.vonMises[cellIndex(field, 1, 1, 2)]).toBe(11);
    expect(field.solid[cellIndex(field, 0, 0, 1)]).toBe(0);
    expect(field.solid[cellIndex(field, 1, 0, 1)]).toBe(1);
    expect(field.displacement[5]).toBeCloseTo(0.5, 6);
    expect(cellAt(field, [13, 23, 5])).toBe(cellIndex(field, 1, 1, 2));
    expect(cellAt(field, [9.9, 20, 0])).toBeNull();
    expect(cellAt(field, [14, 20, 0])).toBeNull();
  });

  it("verwirft einen Block, der nicht zum Gitter passt", () => {
    const field = allocateField(summary([2, 2, 3]));
    const ok = chunk(0, 0, 4, () => ({ solid: 1, vm: 0, sf: 1, d: 0 }));
    expect(() => applyChunk(field, { ...ok, count: 3 })).toThrow(/Ungültiger Ergebnisblock/);
    expect(() => applyChunk(field, { ...ok, z_to: 5, count: 24 })).toThrow(/Ungültiger Ergebnisblock/);
    expect(() => applyChunk(field, { ...ok, von_mises: f32([1, 2]) })).toThrow(/Feldlängen/);
  });
});

describe("Farben", () => {
  const s = summary([1, 1, 1]);

  it("färbt die Spannung über die Rampe und alles über der Streckgrenze in der Grenzfarbe", () => {
    const scale = stressScale("von-mises", s, 45);
    expect(scale.scale).toEqual({ min: 0, max: 100, limit: 45 });
    expect(binFor(scale, 0)).toBe(0);
    expect(binFor(scale, 44)).toBe(RAMP.length - 1);
    expect(binFor(scale, 46)).toBe(-1);
    expect(colorFor(scale, 46)).toBe(OVER_COLOR);
    expect(colorFor(scale, 0)).toBe(RAMP[0]);
  });

  it("färbt die Sicherheit umgekehrt: kritisch hell, unter 1 in der Grenzfarbe, ab dem Deckel dunkel", () => {
    const scale = stressScale("safety", s, 45);
    expect(binFor(scale, 0.99)).toBe(-1);
    expect(colorFor(scale, 0)).toBe(OVER_COLOR);
    expect(binFor(scale, 1)).toBe(RAMP.length - 1);
    expect(binFor(scale, SAFETY_TOP)).toBe(0);
    expect(binFor(scale, 1000)).toBe(0);
    expect(binFor(scale, 2)).toBeGreaterThan(binFor(scale, 4));
    const legend = stressLegend(scale);
    expect(legend[0]!.color).toBe(RAMP[RAMP.length - 1]);
    expect(legend[0]!.from).toBe(1);
    expect(legend[legend.length - 1]!.to).toBe(SAFETY_TOP);
  });

  it("färbt die Verformung von 0 bis zum Maximum und formatiert deutsch", () => {
    const scale = stressScale("displacement", s, 45);
    expect(binFor(scale, 0.2)).toBe(RAMP.length - 1);
    expect(stressLegend(scale)).toHaveLength(RAMP.length);
    expect(formatValue("von-mises", 2.5)).toBe("2,50");
    expect(formatValue("von-mises", 45.04)).toBe("45,0");
    expect(formatValue("safety", 1234)).toBe("1234");
    expect(formatValue("displacement", 0.01234)).toBe("0,0123");
  });
});

describe("Netz der Voxeloberfläche", () => {
  const cube = (n: number) => {
    const field = allocateField(summary([n, n, n], [5, 5, 0]));
    field.solid.fill(1);
    for (let i = 0; i < field.vonMises.length; i += 1) field.vonMises[i] = i;
    return field;
  };
  const scale = (field: ReturnType<typeof cube>) => stressScale("von-mises", { ...summary(field.size), max_von_mises: { value: field.vonMises.length - 1, at: [0, 0, 0] } }, 1e9);

  it("zeichnet von einem Block nur die Außenflächen", () => {
    const field = cube(3);
    const mesh = buildStressMesh(field, scale(field), null);
    expect(mesh.faces).toBe(6 * 9);
    expect(mesh.positions.length).toBe(mesh.faces * 12);
    expect(mesh.indices.length).toBe(mesh.faces * 6);
    expect(Math.max(...mesh.indices)).toBe(mesh.faces * 4 - 1);
    // Alle Ecken liegen im Gitter.
    for (let i = 0; i < mesh.positions.length; i += 3) {
      expect(mesh.positions[i]!).toBeGreaterThanOrEqual(5);
      expect(mesh.positions[i]!).toBeLessThanOrEqual(11);
      expect(mesh.positions[i + 2]!).toBeLessThanOrEqual(6);
    }
  });

  it("legt den Schnitt frei: darunter bleibt der Block, die Schnittfläche trägt die Werte des Inneren", () => {
    const field = cube(3);
    const cut = buildStressMesh(field, scale(field), 1);
    // 3x3x2 Block: 2*(9) Deckel und Boden + 4 Seiten mit 3x2 Flächen
    expect(cut.faces).toBe(2 * 9 + 4 * 6);
    let top = 0;
    for (let i = 2; i < cut.positions.length; i += 12) if (cut.positions[i]! === 4 && cut.positions[i + 3]! === 4) top += 1;
    expect(top).toBeGreaterThan(0);
    expect(buildStressMesh(field, scale(field), -1).faces).toBe(0);
  });

  it("lässt Hohlräume offen: eine fehlende Mittelzelle macht ihre sechs Wände sichtbar", () => {
    const field = cube(3);
    field.solid[cellIndex(field, 1, 1, 1)] = 0;
    expect(buildStressMesh(field, scale(field), null).faces).toBe(6 * 9 + 6);
  });

  it("färbt jede Fläche in der Farbe ihrer Zelle und die Zellen über der Streckgrenze in der Grenzfarbe", () => {
    const field = allocateField(summary([2, 1, 1]));
    field.solid.fill(1);
    field.vonMises.set([10, 60]);
    const limited = stressScale("von-mises", { ...summary([2, 1, 1]), max_von_mises: { value: 60, at: [0, 0, 0] } }, 45);
    const mesh = buildStressMesh(field, limited, null);
    expect(mesh.faces).toBe(10);
    const colors = new Set<string>();
    for (let f = 0; f < mesh.faces; f += 1) colors.add([...mesh.colors.slice(f * 12, f * 12 + 3)].map((v) => v.toFixed(3)).join(","));
    expect(colors.size).toBe(2);
  });
});

// Eine Platte aus zwei Dreiecken oben (z = 1), zwei darunter (z = 0) und eine Schräge.
function quadMesh() {
  const positions = new Float32Array([
    0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, // 0..3 Boden
    0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, // 4..7 Deckel
    2, 0, 1, 2, 1, 1, // 8,9 (wird zur Schräge)
    2, 0, 2, 2, 1, 2, // 10,11 Rampe nach oben
  ]);
  const indices = new Uint32Array([
    0, 2, 1, 0, 3, 2, // Boden, Normale -z
    4, 5, 6, 4, 6, 7, // Deckel, Normale +z
    5, 8, 9, 5, 9, 6, // Verlängerung des Deckels (eben)
    8, 10, 11, 8, 11, 9, // senkrechte Wand: Normale -x
  ]);
  return { positions, indices };
}

describe("Flächenwahl", () => {
  it("wählt die ebene Fläche samt allen zusammenhängenden Nachbarn gleicher Normale", () => {
    const { positions, indices } = quadMesh();
    expect(planarRegion(positions, indices, 2)).toEqual([2, 3, 4, 5]);
    expect(planarRegion(positions, indices, 0)).toEqual([0, 1]);
  });

  it("hört an der Kante auf, wo die Normale abknickt", () => {
    const { positions, indices } = quadMesh();
    expect(planarRegion(positions, indices, 6)).toEqual([6, 7]);
    expect(planarRegion(positions, indices, 99)).toEqual([]);
  });

  it("verbindet auch Netze, deren Eckpunkte nicht geteilt sind", () => {
    const positions = new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, /* zweites Dreieck, eigene Punkte */ 0, 0, 0, 1, 1, 0, 0, 1, 0]);
    const indices = new Uint32Array([0, 1, 2, 3, 4, 5]);
    expect(planarRegion(positions, indices, 0)).toEqual([0, 1]);
  });

  it("zählt Dreiecke über alle Teile durch, wie die Engine", () => {
    const parts = [{ indices: new Uint32Array(9) }, { indices: new Uint32Array(15) }];
    expect(partOffsets(parts)).toEqual([0, 3]);
    expect(globalTriangle(parts, 1, 2)).toBe(5);
    expect(globalTriangle(parts, 1, 5)).toBeNull();
    expect(globalTriangle(parts, 2, 0)).toBeNull();
  });

  it("vereinigt und zieht Auswahlen ab", () => {
    expect(mergeTriangles([3, 1], [2, 3])).toEqual([1, 2, 3]);
    expect(subtractTriangles([1, 2, 3, 4], [2, 4])).toEqual([1, 3]);
  });
});
