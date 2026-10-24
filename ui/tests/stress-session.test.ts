// Die Kraftverteilung durch die Session gegen die echte ananas-engine: Würfel von 20 mm, unten fest, oben belastet.

import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession, stressResultCurrent } from "../src/state/session.ts";
import { MATERIAL_PRESETS } from "../src/stress/materials.ts";
import { cellAt } from "../src/stress/field.ts";
import { MODEL, startEngine } from "./engineHarness.ts";

const engine = startEngine();
let session: CockpitSession;

beforeAll(async () => {
  session = new CockpitSession(new EngineClient(engine));
  await session.start();
  session.setModel(MODEL);
});

afterAll(async () => {
  await engine.close();
});

const state = () => session.getState();
const box = () => state().objects[0]!.box;

function setUp(force: [number, number, number]) {
  session.openStress();
  const support = session.addStressRegion("support");
  const load = session.addStressRegion("load");
  const b = box();
  session.updateStressRegion(support, { box: { min: [b.min[0], b.min[1], b.min[2]], max: [b.max[0], b.max[1], b.min[2]] } });
  session.updateStressRegion(load, { box: { min: [b.min[0], b.min[1], b.max[2]], max: [b.max[0], b.max[1], b.max[2]] }, force });
  session.setStressVoxelSize(2);
}

describe("Kraftverteilung am Würfel", () => {
  it("rechnet Druck von oben, meldet Fortschritt und liefert ein Gitter", async () => {
    setUp([0, 0, -1000]);
    const percents: number[] = [];
    const unsubscribe = session.subscribe(() => {
      const run = state().stress.run;
      if (run.status === "running") percents.push(run.percent);
    });
    await session.runStress();
    unsubscribe();

    const { stress } = state();
    expect(stress.run.error).toBeNull();
    expect(stress.run.status).toBe("done");
    expect(stress.display.shown).toBe(true);
    expect(percents.length).toBeGreaterThan(1);
    expect(percents[percents.length - 1]).toBe(100);
    expect([...percents].sort((a, b) => a - b)).toEqual(percents);

    const result = stress.result!;
    const { summary, field } = result;
    expect(summary.solver.converged).toBe(true);
    expect(field.size[0] * field.size[1] * field.size[2]).toBe(field.solid.length);
    const solid = field.solid.reduce((n, v) => n + v, 0);
    expect(solid).toBe(summary.solid_cells);

    // Gleichgewicht: die Reaktion hebt die Last auf.
    expect(summary.applied_force[2]).toBeCloseTo(-1000, 3);
    expect(summary.reaction[2]).toBeCloseTo(1000, 0);
    // Mittlere Spannung F/A = 1000 N / 400 mm² = 2,5 MPa; die Spitzen liegen darüber, aber in derselben Größenordnung.
    expect(summary.max_von_mises.value).toBeGreaterThan(2.4);
    expect(summary.max_von_mises.value).toBeLessThan(10);
    // Stauchung F·L/(E_z·A) mit E_z = z-Faktor·E = 0,6·3000 MPa: 1000·20/(1800·400) ≈ 0,0278 mm.
    expect(summary.max_displacement.value).toBeGreaterThan(0.025);
    expect(summary.max_displacement.value).toBeLessThan(0.031);
    // Das Maximum der Verformung ist ein Knoten oben; die Zelle eine halbe Zelle nach innen trägt einen Wert nahe daran.
    const half = field.voxelSize / 2;
    const at = summary.max_displacement.at;
    // Die Verformung ist am Rand fast gleichmäßig, das Maximum kann an jedem Knoten der Oberseite liegen (auch am Rand
    // des Gitters): die Zelle liegt deshalb eine halbe Zelle zur Mitte des Gitters hin.
    const inward = (d: 0 | 1 | 2): number => at[d] + (at[d] < field.origin[d]! + (field.size[d] * field.voxelSize) / 2 ? half : -half);
    const top = cellAt(field, [inward(0), inward(1), inward(2)])!;
    expect(top).not.toBeNull();
    expect(field.displacement[top]).toBeGreaterThan(summary.max_displacement.value * 0.9);
    expect(stressResultCurrent(state())).toBe(true);
  });

  it("erklärt das Ergebnis für veraltet, sobald das Objekt verschoben wird", async () => {
    await session.setObjectValue("object.position.x", state().objects[0]!.transform.position[0] + 5);
    expect(stressResultCurrent(state())).toBe(false);
    await session.setObjectValue("object.position.x", state().objects[0]!.transform.position[0] - 5);
    expect(stressResultCurrent(state())).toBe(true);
  });

  it("übernimmt den Werkstoff: E_z geht mit E-Modul und z-Faktor in die Verformung ein", async () => {
    const before = state().stress.result!.summary.max_displacement.value;
    const pla = MATERIAL_PRESETS.find((preset) => preset.id === "pla")!;
    session.setStressMaterial({ youngsModulus: pla.youngsModulus / 2, zFactor: 1 });
    await session.runStress();
    expect(state().stress.run.status).toBe("done");
    // E_z fällt von 1800 auf 1500 MPa: die Verformung steigt um den Faktor 1,2.
    expect(state().stress.result!.summary.max_displacement.value / before).toBeCloseTo(1.2, 1);
  });

  it("zeigt die Ablehnung der Engine, wenn ein Lager nichts trifft", async () => {
    const support = state().stress.setup.regions.find((region) => region.kind === "support")!;
    session.updateStressRegion(support.id, { box: { min: [0, 0, 500], max: [1, 1, 501] } });
    await session.runStress();
    const run = state().stress.run;
    expect(run.status).toBe("failed");
    expect(run.error).toMatch(/select|support|empty/i);
    expect(state().stress.result).toBeNull();
  });

  it("verweigert ein Lager ohne festgehaltene Achse schon vor der Engine", async () => {
    const support = state().stress.setup.regions.find((region) => region.kind === "support")!;
    session.updateStressRegion(support.id, { axes: [false, false, false] });
    await session.runStress();
    expect(state().stress.run.error).toContain("keine Achse");
  });

  it("verweigert den Start ohne Last, ohne die Engine zu fragen", async () => {
    const support = state().stress.setup.regions.find((region) => region.kind === "support")!;
    session.updateStressRegion(support.id, { axes: [true, true, true] });
    const load = state().stress.setup.regions.find((region) => region.kind === "load")!;
    session.updateStressRegion(load.id, { force: [0, 0, 0] });
    await session.runStress();
    expect(state().stress.run.status).toBe("failed");
    expect(state().stress.run.error).toContain("0 N");
  });

  it("wählt Flächen per Klick: dieselbe Rechnung wie mit Boxen, über die Dreiecksindizes der Engine", async () => {
    session.discardStressResult();
    const reference = (() => {
      const [support, load] = state().stress.setup.regions;
      return { support: support!.id, load: load!.id };
    })();
    // Zurück zu einer gültigen Einrichtung (Boxen), Referenzrechnung.
    const b = box();
    session.updateStressRegion(reference.support, { box: { min: [b.min[0], b.min[1], b.min[2]], max: [b.max[0], b.max[1], b.min[2]] } });
    session.updateStressRegion(reference.load, { force: [0, 0, -1000] });
    await session.runStress();
    const byBox = state().stress.result!.summary;

    // Dieselben Flächen als Dreiecke: das Dreieck der Unterseite und das der Oberseite im Netz suchen.
    const part = state().objects[0]!.parts[0]!;
    const rawBox = state().objects[0]!.rawBox;
    const triangleAt = (z: number): number => {
      for (let t = 0; t < part.indices.length / 3; t += 1) {
        if ([0, 1, 2].every((k) => Math.abs(part.positions[part.indices[t * 3 + k]! * 3 + 2]! - z) < 1e-4)) return t;
      }
      throw new Error("kein Dreieck auf dieser Höhe");
    };
    session.updateStressRegion(reference.support, { box: null });
    session.updateStressRegion(reference.load, { box: null });
    session.armStressRegion(reference.support);
    session.pickStressFace(0, 0, triangleAt(rawBox.min[2]));
    session.armStressRegion(reference.load);
    session.pickStressFace(0, 0, triangleAt(rawBox.max[2]));
    const regions = state().stress.setup.regions;
    expect(regions[0]!.triangles.length).toBe(2); // eine Würfelfläche besteht aus zwei Dreiecken
    expect(regions[1]!.triangles.length).toBe(2);
    expect(regions[0]!.triangles).not.toEqual(regions[1]!.triangles);

    await session.runStress();
    expect(state().stress.run.error).toBeNull();
    const byTriangles = state().stress.result!.summary;
    expect(byTriangles.reaction[2]).toBeCloseTo(byBox.reaction[2], 0);
    expect(byTriangles.max_displacement.value / byBox.max_displacement.value).toBeGreaterThan(0.95);
    expect(byTriangles.max_displacement.value / byBox.max_displacement.value).toBeLessThan(1.05);

    // Ein zweiter Klick auf dieselbe Fläche nimmt sie wieder heraus.
    session.armStressRegion(reference.load);
    session.pickStressFace(0, 0, triangleAt(rawBox.max[2]));
    expect(state().stress.setup.regions[1]!.triangles).toEqual([]);
  });
});
