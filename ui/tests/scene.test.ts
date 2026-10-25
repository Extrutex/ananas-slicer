// The model on the bed and the G-code preview, driven through the session against the real engine.

import { join } from "node:path";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession } from "../src/state/session.ts";
import { fitsBed } from "../src/viewer/bed.ts";
import { sizeOf } from "../src/viewer/objects.ts";
import { MODEL, startEngine } from "./engineHarness.ts";

const engine = startEngine();
const data = (name: string) => join(import.meta.dirname, "../../tests/data", name);
const repoFile = (name: string) => join(import.meta.dirname, "../..", name);
let session: CockpitSession;

beforeAll(async () => {
  session = new CockpitSession(new EngineClient(engine));
  await session.start();
});

afterAll(async () => {
  await engine.close();
});

const state = () => session.getState();
const object = (index = 0) => {
  const found = state().objects[index];
  if (found === undefined) throw new Error(`no object ${index}`);
  return found;
};
const centre = (index = 0): [number, number] => [(object(index).box.min[0] + object(index).box.max[0]) / 2, (object(index).box.min[1] + object(index).box.max[1]) / 2];

describe("the sample model", () => {
  it("is on the bed after start, in the middle, set down and selected", () => {
    expect(state().objects).toHaveLength(1);
    expect(state().selected).toBe(0);
    expect(state().modelError).toBeNull();
    const [x, y] = centre();
    expect(x).toBeCloseTo(state().bed.center[0], 2);
    expect(y).toBeCloseTo(state().bed.center[1], 2);
    expect(object().box.min[2]).toBeCloseTo(0, 4);
    expect(fitsBed(object().box, state().bed)).toBe(true);
  });

  it("knows the printer's bed", () => {
    expect(state().bed.polygon).toHaveLength(4);
    expect(state().bed.height).toBeGreaterThan(100);
  });
});

describe("placing the object", () => {
  it("applies a typed rotation and shows the resulting size", async () => {
    await session.setObjectValue("object.rotation.z", 45);
    expect(object().transform.rotation[2]).toBe(45);
    expect(sizeOf(object().box)[0]).toBeCloseTo(28.28, 1);
    await session.setObjectValue("object.rotation.z", 0);
    expect(sizeOf(object().box)[0]).toBeCloseTo(20, 1);
  });

  it("scales every axis together while uniform scale is on, and one axis without it", async () => {
    await session.setObjectValue("object.scale.x", 200);
    expect(object().transform.scale).toEqual([2, 2, 2]);
    expect(sizeOf(object().box)[2]).toBeCloseTo(40, 1);
    session.setUniformScale(false);
    await session.setObjectValue("object.scale.z", 50);
    expect(object().transform.scale).toEqual([2, 2, 0.5]);
    expect(sizeOf(object().box)[2]).toBeCloseTo(10, 1);
    session.setUniformScale(true);
    await session.setObjectValue("object.scale.x", 100);
    expect(object().transform.scale).toEqual([1, 1, 1]);
  });

  it("refuses a value the engine rejects, keeps the old placement and marks the row", async () => {
    const before = object().transform.scale;
    await session.setObjectValue("object.scale.x", 0);
    expect(object().transform.scale).toEqual(before);
    expect(state().invalid["object.scale.x"]).toContain("scale");
  });

  it("follows a drag on screen and lets the engine confirm it afterwards", async () => {
    const start = [...object().transform.position] as [number, number];
    session.moveObject(0, [start[0] + 30, start[1] - 10]);
    expect(object().transform.position).toEqual([start[0] + 30, start[1] - 10]);
    expect(centre()[0]).toBeCloseTo(start[0] + 30, 2);
    await session.commitObject();
    expect(centre()[0]).toBeCloseTo(start[0] + 30, 2);
    expect(centre()[1]).toBeCloseTo(start[1] - 10, 2);
    expect(object().box.min[2]).toBeCloseTo(0, 4);
  });

  it("notices an object that has left the bed", async () => {
    await session.setObjectValue("object.position.x", 900);
    expect(session.fitsBed).toBe(false);
    await session.setObjectValue("object.position.x", state().bed.center[0]);
    expect(session.fitsBed).toBe(true);
  });

  it("selects and deselects", () => {
    session.selectObject(null);
    expect(state().selected).toBeNull();
    session.selectObject(0);
    expect(state().selected).toBe(0);
  });
});

describe("other models", () => {
  it("keeps the layout of a file with several objects, none selected", async () => {
    await session.loadModel(repoFile("resources/calib/filament_flow/pass1.3mf"));
    expect(state().objects).toHaveLength(9);
    expect(state().selected).toBeNull();
    expect(session.fitsBed).toBe(true);
    const spread = new Set(state().objects.map((o) => o.transform.position[0].toFixed(0)));
    expect(spread.size).toBeGreaterThan(1);
  });

  it("reports a file it cannot read and keeps the session usable", async () => {
    await session.loadModel("/no/such/model.stl");
    expect(state().modelError).toContain("model file not found");
    expect(state().objects).toHaveLength(0);
    await session.loadModel(MODEL);
    expect(state().modelError).toBeNull();
    expect(state().objects).toHaveLength(1);
  });

  it("returns the parts of a multi-part model", async () => {
    await session.loadModel(repoFile("resources/handy_models/OrcaSliced.3mf"));
    expect(state().objects).toHaveLength(3);
    expect(state().objects[2]!.parts.length).toBeGreaterThan(10);
    await session.loadModel(MODEL);
  });
});

describe("the view", () => {
  it("switches between the three model views and the explosion amount", () => {
    session.setViewMode("inspect");
    expect(state().view.mode).toBe("inspect");
    session.setViewMode("mesh");
    session.setExplode(0.5);
    expect(state().view.explode).toBe(0.5);
    session.setViewMode("solid");
    session.setExplode(0);
  });
});

describe("the G-code preview", () => {
  it("shows the toolpaths of a slice, layer by layer", async () => {
    session.setModel(data("20mm_cube.obj"));
    await session.loadModel(data("20mm_cube.obj"));
    const result = await session.slice();
    expect(result).not.toBeNull();
    // The preview loads after the slice and is queued behind it.
    await session.loadPreview(result!.gcode);
    const { preview, view } = state();
    expect(preview.status).toBe("ready");
    expect(view.target).toBe("gcode");
    expect(preview.data!.summary.layers).toHaveLength(100);
    expect(preview.data!.extrusion.count).toBeGreaterThan(5000);
    expect(preview.data!.travel.count).toBeGreaterThan(0);
    expect(preview.layerMin).toBe(0);
    expect(preview.layerMax).toBe(99);
    const starts = preview.data!.extrusion.layerStart;
    expect(starts).toHaveLength(101);
    expect(starts[100]).toBe(preview.data!.extrusion.count);
    expect([...starts].every((value, i, all) => i === 0 || value >= all[i - 1]!)).toBe(true);
  });

  it("places the toolpaths where the object stands", async () => {
    const box = state().preview.data!.summary.bbox!;
    const [x, y] = [(box.min[0] + box.max[0]) / 2, (box.min[1] + box.max[1]) / 2];
    expect(x).toBeCloseTo(state().bed.center[0], 0);
    expect(y).toBeCloseTo(state().bed.center[1], 0);
  });

  it("slices a model where the user put it", async () => {
    await session.setObjectValue("object.position.x", 100);
    await session.setObjectValue("object.position.y", 120);
    const result = await session.slice();
    await session.loadPreview(result!.gcode);
    const box = state().preview.data!.summary.bbox!;
    expect((box.min[0] + box.max[0]) / 2).toBeCloseTo(100, 0);
    expect((box.min[1] + box.max[1]) / 2).toBeCloseTo(120, 0);
  });

  it("holds the layer range, quantity and travel switch", () => {
    session.setPreview({ layerMax: 10, quantity: "accel", showTravel: true, explodeLayers: 4 });
    expect(state().preview).toMatchObject({ layerMax: 10, quantity: "accel", showTravel: true, explodeLayers: 4 });
  });
});
