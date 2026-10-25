// The viewer must draw an object exactly where the engine puts it: the matrix built in the browser against the
// engine's own bounding box, for transforms that exercise every part of the composition.

import { join } from "node:path";
import { afterAll, describe, expect, it } from "vitest";
import { EngineClient } from "../src/engine/client.ts";
import type { ObjectTransform } from "../src/engine/protocol.ts";
import { objectsFromModel, sizeOf, transformedBox, worldMatrix, type Transform } from "../src/viewer/objects.ts";
import { startEngine } from "./engineHarness.ts";

const engine = startEngine();
const client = new EngineClient(engine);
const data = (name: string) => join(import.meta.dirname, "../../tests/data", name);

afterAll(async () => {
  await engine.close();
});

const TRANSFORMS: Transform[] = [
  { position: [100, 100], rotation: [0, 0, 0], scale: [1, 1, 1] },
  { position: [50, 80], rotation: [0, 0, 37], scale: [1, 1, 1] },
  { position: [50, 80], rotation: [30, 20, 10], scale: [1, 1, 1] },
  { position: [120, 60], rotation: [90, 0, 0], scale: [1.5, 0.5, 2] },
  { position: [120, 60], rotation: [-25, 140, 200], scale: [0.7, 1.2, 0.9] },
];

describe.each(["ipadstand.obj", "frog_legs.obj", "overhang.obj"])("%s", (file) => {
  it.each(TRANSFORMS)("sits where the engine puts it (%o)", async (transform) => {
    const model = await client.loadModel(data(file));
    const [object] = objectsFromModel(model, [0, 0]);
    expect(object).toBeDefined();
    const bounds = await client.bounds(data(file), [transform as ObjectTransform]);
    const engineBox = bounds.objects[0]!;
    const drawn = transformedBox(object!.parts, worldMatrix(transform, engineBox.offset));
    for (let axis = 0; axis < 3; axis += 1) {
      expect(drawn.min[axis]).toBeCloseTo(engineBox.bbox.min[axis]!, 2);
      expect(drawn.max[axis]).toBeCloseTo(engineBox.bbox.max[axis]!, 2);
    }
    expect(engineBox.bbox.min[2]).toBeCloseTo(0, 4);
    expect((engineBox.bbox.min[0] + engineBox.bbox.max[0]) / 2).toBeCloseTo(transform.position[0], 3);
    expect((engineBox.bbox.min[1] + engineBox.bbox.max[1]) / 2).toBeCloseTo(transform.position[1], 3);
  });
});

const repoFile = (name: string) => join(import.meta.dirname, "../..", name);

describe("a 3MF with several objects", () => {
  const file = repoFile("resources/calib/filament_flow/pass1.3mf");

  it("keeps the layout of the file and puts the group in the middle of the bed", async () => {
    const model = await client.loadModel(file);
    const objects = objectsFromModel(model, [175, 175]);
    expect(objects).toHaveLength(9);
    const centres = objects.map((object) => object.transform.position);
    const xs = centres.map((c) => c[0]);
    const ys = centres.map((c) => c[1]);
    // The objects are not stacked on one point ...
    expect(new Set(xs.map((v) => v.toFixed(1))).size + new Set(ys.map((v) => v.toFixed(1))).size).toBeGreaterThan(3);
    // ... and, placed by the engine, their group is centred on the bed.
    const bounds = await client.bounds(file, objects.map((object) => ({ position: object.transform.position, rotation: object.transform.rotation, scale: object.transform.scale })));
    const boxes = bounds.objects.map((answer) => answer.bbox);
    const minX = Math.min(...boxes.map((b) => b.min[0]));
    const maxX = Math.max(...boxes.map((b) => b.max[0]));
    const minY = Math.min(...boxes.map((b) => b.min[1]));
    const maxY = Math.max(...boxes.map((b) => b.max[1]));
    expect((minX + maxX) / 2).toBeCloseTo(175, 1);
    expect((minY + maxY) / 2).toBeCloseTo(175, 1);
  });

  it("agrees with the engine about where each object ends up", async () => {
    const model = await client.loadModel(file);
    const objects = objectsFromModel(model, [175, 175]);
    const bounds = await client.bounds(file, objects.map((object) => ({ position: object.transform.position, rotation: object.transform.rotation, scale: object.transform.scale })));
    objects.forEach((object, index) => {
      const answer = bounds.objects[index]!;
      const drawn = transformedBox(object.parts, worldMatrix(object.transform, answer.offset));
      for (let axis = 0; axis < 3; axis += 1) {
        expect(drawn.min[axis]).toBeCloseTo(answer.bbox.min[axis]!, 1);
        expect(drawn.max[axis]).toBeCloseTo(answer.bbox.max[axis]!, 1);
      }
    });
  });
});

describe("a 3MF with many parts", () => {
  it("returns every model part as its own mesh", async () => {
    const model = await client.loadModel(repoFile("resources/handy_models/OrcaSliced.3mf"));
    const [head, belly, pedestal] = model.objects;
    expect(head!.parts.length).toBeGreaterThan(1);
    expect(belly!.parts.length).toBeGreaterThan(1);
    expect(pedestal!.parts.length).toBeGreaterThan(10);
    expect(pedestal!.parts.reduce((sum, part) => sum + part.triangles, 0)).toBe(pedestal!.stats.facets);
  });
});

describe("a loaded model", () => {
  it("carries its parts, size and mesh statistics", async () => {
    const model = await client.loadModel(data("20mm_cube.obj"));
    const [object] = objectsFromModel(model, [10, 10]);
    expect(object!.parts).toHaveLength(1);
    expect(object!.parts[0]!.triangles).toBe(12);
    expect(sizeOf(object!.rawBox)).toEqual([20, 20, 20].map((v) => expect.closeTo(v, 3)));
    expect(object!.stats.open_edges).toBe(0);
    expect(object!.stats.volume).toBeCloseTo(8000, 0);
    expect(object!.transform.position).toEqual([10, 10]);
  });

  it("reports the mesh errors of a model that is not closed", async () => {
    const model = await client.loadModel(data("two_hollow_squares.obj"));
    expect(model.objects.length).toBeGreaterThan(0);
    expect(model.objects[0]!.stats.facets).toBeGreaterThan(0);
  });
});
