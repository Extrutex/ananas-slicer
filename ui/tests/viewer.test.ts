import { describe, expect, it } from "vitest";
import { Matrix4 } from "three";
import { fitsBed, parseBed, pointInPolygon } from "../src/viewer/bed.ts";
import { OVER_COLOR, RAMP, binOf, countOver, legendBins } from "../src/viewer/heatmap.ts";
import { classifyFaces } from "../src/viewer/inspect.ts";
import { rowMajor, worldMatrix } from "../src/viewer/objects.ts";
import { mergeChunks } from "../src/viewer/preview.ts";
import type { PreviewChunkJson, PreviewSummary } from "../src/engine/protocol.ts";

describe("the bed", () => {
  const bed = parseBed("0x0,350x0,350x350,0x350", "340");

  it("reads the printable area and height", () => {
    expect(bed.polygon).toHaveLength(4);
    expect(bed.center).toEqual([175, 175]);
    expect(bed.height).toBe(340);
  });

  it("falls back to a 220 mm square for a value it cannot read", () => {
    expect(parseBed("nonsense", undefined).max).toEqual([220, 220]);
    expect(parseBed(undefined, "x").height).toBe(250);
  });

  it("tells inside from outside, an outline point counting as inside", () => {
    expect(pointInPolygon([10, 10], bed.polygon)).toBe(true);
    expect(pointInPolygon([0, 100], bed.polygon)).toBe(true);
    expect(pointInPolygon([-0.1, 100], bed.polygon)).toBe(false);
  });

  it("follows a bed that is not a rectangle", () => {
    const round = parseBed("0x0,100x0,100x50,50x100,0x100", "100");
    expect(pointInPolygon([90, 90], round.polygon)).toBe(false);
    expect(pointInPolygon([40, 40], round.polygon)).toBe(true);
  });

  it("fits an object whose footprint and height are inside", () => {
    expect(fitsBed({ min: [10, 10, 0], max: [30, 30, 20] }, bed)).toBe(true);
    expect(fitsBed({ min: [-5, 10, 0], max: [15, 30, 20] }, bed)).toBe(false);
    expect(fitsBed({ min: [10, 10, 0], max: [30, 30, 400] }, bed)).toBe(false);
  });
});

describe("the heatmap", () => {
  const scale = { min: 0, max: 100, limit: null };

  it("has eleven bins and puts the lowest value in the first and the highest in the last", () => {
    expect(RAMP).toHaveLength(11);
    expect(binOf(0, scale)).toBe(0);
    expect(binOf(100, scale)).toBe(10);
    expect(binOf(50, scale)).toBe(5);
  });

  it("puts a value above the limit out of the ramp", () => {
    const limited = { min: 0, max: 400, limit: 300 };
    expect(binOf(300, limited)).toBe(10);
    expect(binOf(301, limited)).toBe(-1);
    expect(OVER_COLOR).toBe("#ff6f5e");
  });

  it("spreads the ramp over the range below the limit", () => {
    const bins = legendBins({ min: 0, max: 400, limit: 300 });
    expect(bins[0]!.from).toBe(0);
    expect(bins.at(-1)!.to).toBeCloseTo(300);
  });

  it("puts everything in the middle when the range is flat", () => {
    expect(binOf(7, { min: 7, max: 7, limit: null })).toBe(5);
  });

  it("counts the values above the limit", () => {
    expect(countOver([100, 301, 400, 299], { min: 0, max: 400, limit: 300 })).toBe(2);
    expect(countOver([1, 2], scale)).toBe(0);
  });
});

// A unit cube from -0.5 to 0.5, twelve triangles, outward normals.
const cube = {
  positions: new Float32Array([
    -0.5, -0.5, -0.5, 0.5, -0.5, -0.5, 0.5, 0.5, -0.5, -0.5, 0.5, -0.5, -0.5, -0.5, 0.5, 0.5, -0.5, 0.5, 0.5, 0.5, 0.5, -0.5, 0.5, 0.5,
  ]),
  indices: new Uint32Array([
    0, 3, 2, 0, 2, 1, // bottom (-z)
    4, 5, 6, 4, 6, 7, // top (+z)
    0, 1, 5, 0, 5, 4, // front (-y)
    2, 3, 7, 2, 7, 6, // back (+y)
    1, 2, 6, 1, 6, 5, // right (+x)
    3, 0, 4, 3, 4, 7, // left (-x)
  ]),
};

function classify(rotation: [number, number, number], threshold: number, lift = 0.5) {
  const world = worldMatrix({ position: [0, 0], rotation, scale: [1, 1, 1] }, [0, 0, lift]);
  return classifyFaces(cube.positions, cube.indices, rowMajor(world), threshold);
}

describe("the inspection view", () => {
  it("marks the bottom of an upright cube as bed contact and the rest as fine", () => {
    const result = classify([0, 0, 0], 45);
    expect(result.share.contact).toBeCloseTo(1 / 6);
    expect(result.share.ok).toBeCloseTo(5 / 6);
    expect(result.share.overhang).toBe(0);
  });

  it("marks a face that points down and is not on the bed as an overhang", () => {
    // Lifted off the bed, the bottom is a ceiling.
    const result = classify([0, 0, 0], 45, 10);
    expect(result.share.overhang).toBeCloseTo(1 / 6);
    expect(result.share.contact).toBe(0);
  });

  it("measures the slope from the horizontal against the profile's threshold", () => {
    // Tilted 45 degrees about X, the lowest faces slope at 45 degrees, standing on an edge.
    const tilted = (threshold: number) => classify([45, 0, 0], threshold, Math.SQRT1_2);
    expect(tilted(60).share.overhang).toBeCloseTo(2 / 6);
    expect(tilted(30).share.overhang).toBe(0);
    expect(tilted(30).share.ok).toBeGreaterThan(0.9);
  });

  it("marks the band just above the threshold as a caution", () => {
    const result = classify([45, 0, 0], 40, Math.SQRT1_2);
    expect(result.share.caution).toBeCloseTo(2 / 6);
    expect(result.share.overhang).toBe(0);
  });

  it("weights the shares by area and they add up to one", () => {
    const world = worldMatrix({ position: [0, 0], rotation: [0, 0, 0], scale: [4, 1, 1] }, [0, 0, 0.5]);
    const result = classifyFaces(cube.positions, cube.indices, rowMajor(world), 45);
    const total = result.share.ok + result.share.caution + result.share.overhang + result.share.contact;
    expect(total).toBeCloseTo(1);
    // 4 x 1 bottom out of a surface of 2*(4*1) + 2*(4*1) + 2*(1*1).
    expect(result.share.contact).toBeCloseTo(4 / 18);
  });

  it("classifies every triangle", () => {
    expect(classify([0, 0, 0], 45).classes).toHaveLength(12);
  });
});

describe("the toolpath arrays", () => {
  const summary = {
    layers: [
      { id: 1, z: 0.2 },
      { id: 2, z: 0.4 },
      { id: 3, z: 0.6 },
    ],
  } as PreviewSummary;

  const b64 = (array: Float32Array | Uint8Array | Uint16Array): string =>
    Buffer.from(array.buffer, array.byteOffset, array.byteLength).toString("base64");

  const chunk = (layers: number[], kinds: number[]): PreviewChunkJson => ({
    count: layers.length,
    positions: b64(new Float32Array(layers.flatMap((layer, i) => [i, 0, layer, i + 1, 0, layer]))),
    speed: b64(new Float32Array(layers.map((_, i) => 10 + i))),
    flow: b64(new Float32Array(layers.map(() => 1))),
    accel: b64(new Float32Array(layers.map(() => 1000))),
    role: b64(new Uint8Array(layers.map(() => 2))),
    kind: b64(new Uint8Array(kinds)),
    layer: b64(new Uint16Array(layers)),
  });

  it("splits extrusions from travel moves and finds where each layer starts", () => {
    const data = mergeChunks(summary, [chunk([0, 0, 1], [0, 1, 0]), chunk([2, 2], [0, 0])]);
    expect(data.extrusion.count).toBe(4);
    expect(data.travel.count).toBe(1);
    expect([...data.extrusion.layerStart]).toEqual([0, 1, 2, 4]);
    expect([...data.travel.layerStart]).toEqual([0, 1, 1, 1]);
  });

  it("keeps the values of a segment together", () => {
    const data = mergeChunks(summary, [chunk([0, 1], [0, 0])]);
    expect([...data.extrusion.speed]).toEqual([10, 11]);
    expect(data.extrusion.positions[2]).toBe(0);
    expect(data.extrusion.positions[8]).toBe(1);
  });

  it("gives empty sets for a file without segments", () => {
    const data = mergeChunks(summary, [{ count: 0 }]);
    expect(data.extrusion.count).toBe(0);
    expect([...data.extrusion.layerStart]).toEqual([0, 0, 0, 0]);
  });
});

describe("the object matrix", () => {
  it("composes translation, Rz*Ry*Rx and scale like the slicer", () => {
    const matrix = worldMatrix({ position: [0, 0], rotation: [0, 0, 90], scale: [2, 1, 1] }, [10, 20, 0]);
    const point = new Matrix4().copy(matrix);
    // (1,0,0) scales to (2,0,0), turns 90 deg about Z to (0,2,0), moves to (10,22,0).
    const e = point.elements;
    const x = e[0]! * 1 + e[12]!;
    const y = e[1]! * 1 + e[13]!;
    expect(x).toBeCloseTo(10);
    expect(y).toBeCloseTo(22);
  });
});
