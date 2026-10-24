import { describe, expect, it } from "vitest";
import {
  checkDraft,
  countStates,
  evaluateLimit,
  formatNumber,
  limitMessage,
  parseNumber,
  steppedValue,
  type CockpitParam,
} from "../src/cockpit/model.ts";
import { buildGroups, firstNumber } from "../src/cockpit/params.ts";
import type { ResolveResult, SchemaOption } from "../src/engine/protocol.ts";

const accel: CockpitParam = {
  id: "outer_wall_acceleration",
  label: "Outer wall",
  unit: "mm/s²",
  value: 3000,
  step: 100,
  decimals: 0,
  min: 0,
  max: 50000,
  limit: { value: 10000, source: "printer.max_accel", kind: "machine" },
};

describe("limit states", () => {
  it("is nominal below 90 percent of the limit", () => {
    expect(evaluateLimit(8999, accel.limit)).toBe("nominal");
  });

  it("is a caution from 90 percent up to the limit", () => {
    expect(evaluateLimit(9000, accel.limit)).toBe("caution");
    expect(evaluateLimit(10000, accel.limit)).toBe("caution");
  });

  it("is exceeded above the limit", () => {
    expect(evaluateLimit(10001, accel.limit)).toBe("exceeded");
  });

  it("is nominal for a row that has nothing to compare with", () => {
    expect(evaluateLimit(1e9, null)).toBe("nominal");
  });

  it("is only a reference for a reference limit, however far the value is off", () => {
    expect(evaluateLimit(1, { value: 0.04, source: "extruder.pressure_advance", kind: "reference" })).toBe("reference");
  });
});

describe("numbers", () => {
  it("groups thousands with a narrow no-break space and keeps the decimals", () => {
    expect(formatNumber(12000, 0)).toBe("12 000");
    expect(formatNumber(0.035, 3)).toBe("0.035");
    expect(formatNumber(-1500, 0)).toBe("−1 500");
  });

  it("parses a German decimal comma, grouping spaces and the minus sign", () => {
    expect(parseNumber("0,035")).toBe(0.035);
    expect(parseNumber("12 000")).toBe(12000);
    expect(parseNumber("12 000")).toBe(12000);
    expect(parseNumber("−5")).toBe(-5);
  });

  it("rejects exponents and text instead of reading them", () => {
    expect(parseNumber("1e4")).toBeNull();
    expect(parseNumber("bad")).toBeNull();
    expect(parseNumber("")).toBeNull();
  });
});

describe("drafts and steps", () => {
  it("rejects an input outside the bounds with the range in the message", () => {
    const check = checkDraft("60000", accel);
    expect(check.ok).toBe(false);
    expect(check.ok === false && check.message).toContain("Outside input range");
  });

  it("rounds an accepted input to the row's decimals", () => {
    const check = checkDraft("2999.6", accel);
    expect(check).toEqual({ ok: true, value: 3000 });
  });

  it("steps by the step size, ten times with Shift, a tenth with Alt", () => {
    expect(steppedValue(accel, 3000, 1, 1)).toBe(3100);
    expect(steppedValue(accel, 3000, -1, 10)).toBe(2000);
    expect(steppedValue({ ...accel, step: 0.5, decimals: 1 }, 5, 1, 0.1)).toBe(5.1);
  });

  it("stops at the input bound and never uses the limit as a clamp", () => {
    expect(steppedValue(accel, 49950, 1, 10)).toBe(50000);
    expect(steppedValue(accel, 10000, 1, 10)).toBe(11000);
    expect(steppedValue(accel, 50, -1, 10)).toBe(0);
  });
});

describe("messages and counts", () => {
  it("words a machine violation and says Klipper does not clamp it", () => {
    const text = limitMessage({ ...accel, value: 12000 }, "exceeded");
    expect(text).toContain("Exceeds printer.max_accel");
    expect(text).toContain("by 2 000");
    expect(text).toContain("Klipper does not clamp");
  });

  it("gives the share of the limit for a caution", () => {
    expect(limitMessage({ ...accel, value: 9500 }, "caution")).toContain("95 % of printer.max_accel");
  });

  it("counts exceeded and caution rows", () => {
    const rows = [accel, { ...accel, value: 9500 }, { ...accel, value: 12000 }, { ...accel, value: 12001 }];
    expect(countStates(rows)).toEqual({ exceeded: 2, caution: 1 });
  });
});

describe("rows from a resolved config", () => {
  const option = (key: string, unit: string, min?: number, max?: number): SchemaOption => ({
    key,
    scope: "print",
    type: "floats",
    vector: true,
    label: key,
    category: "",
    tooltip: "",
    unit,
    mode: "simple",
    readonly: false,
    nullable: false,
    min,
    max,
  });
  const schema = new Map<string, SchemaOption>([
    ["outer_wall_speed", option("outer_wall_speed", "mm/s", 1, 1000)],
    ["outer_wall_acceleration", option("outer_wall_acceleration", "mm/s²")],
    ["pressure_advance", option("pressure_advance", "", 0, 2)],
  ]);
  const resolved: ResolveResult = {
    printer: "P",
    process: "Q",
    filament: "F",
    config: { outer_wall_speed: "200,200", outer_wall_acceleration: "5000", pressure_advance: "0.035" },
    limits: {
      source: "printer.cfg",
      values: {
        max_velocity: 300,
        max_accel: 7000,
        minimum_cruise_ratio: 0.5,
        square_corner_velocity: 5,
        max_z_velocity: 15,
        max_z_accel: 350,
        max_extrude_only_velocity: 1,
        max_extrude_only_accel: 1,
        instantaneous_corner_velocity: 1,
        pressure_advance: 0.04,
        pressure_advance_smooth_time: 0.04,
      },
    },
    violations: [],
  };

  it("takes the first element of a per-extruder value", () => {
    expect(firstNumber("200,180")).toBe(200);
    expect(Number.isNaN(firstNumber(undefined))).toBe(true);
    expect(Number.isNaN(firstNumber(""))).toBe(true);
  });

  it("holds speed rows against max_velocity, acceleration against max_accel and pressure advance against the reference", () => {
    const groups = buildGroups(schema, resolved);
    expect(groups.map((group) => group.id)).toEqual(["speed", "acceleration", "pressure-advance"]);
    const [speed, acceleration, pressure] = groups;
    expect(speed?.params[0]?.limit).toEqual({ value: 300, source: "printer.max_velocity", kind: "machine" });
    expect(acceleration?.params[0]?.limit).toEqual({ value: 7000, source: "printer.max_accel", kind: "machine" });
    expect(pressure?.params[0]?.limit).toEqual({ value: 0.04, source: "extruder.pressure_advance", kind: "reference" });
  });

  it("has no reference value for pressure advance until a printer.cfg or Moonraker snapshot is imported", () => {
    const fromProfile = buildGroups(schema, { ...resolved, limits: { ...resolved.limits!, source: "profile" } });
    expect(fromProfile.find((group) => group.id === "pressure-advance")?.params[0]?.limit).toBeNull();
    expect(fromProfile.find((group) => group.id === "speed")?.params[0]?.limit).not.toBeNull();
  });

  it("names a row by its own label, not the engine's", () => {
    const [speed] = buildGroups(schema, resolved);
    expect(speed?.params[0]?.label).toBe("Outer wall");
  });

  it("takes bounds from the schema and falls back to zero and a large maximum", () => {
    const [speed, acceleration] = buildGroups(schema, resolved);
    expect(speed?.params[0]).toMatchObject({ min: 1, max: 1000 });
    expect(acceleration?.params[0]).toMatchObject({ min: 0, max: 1_000_000 });
  });

  it("leaves out a setting the config does not carry", () => {
    const groups = buildGroups(schema, { ...resolved, config: { outer_wall_speed: "200" } });
    expect(groups.map((group) => group.id)).toEqual(["speed"]);
  });

  it("has no rows without effective limits, because there is nothing to hold them against", () => {
    expect(buildGroups(schema, { ...resolved, limits: null })).toEqual([]);
  });
});
