// The cockpit session against the real ananas-engine.

import { existsSync, readFileSync } from "node:fs";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession } from "../src/state/session.ts";
import { MODEL, PRINTER_CFG, startEngine } from "./engineHarness.ts";

const engine = startEngine();
let session: CockpitSession;

beforeAll(async () => {
  session = new CockpitSession(new EngineClient(engine));
  await session.start();
});

afterAll(async () => {
  await engine.close();
});

function limitOf(id: string) {
  const limit = row(id).limit;
  if (limit === null) throw new Error(`row ${id} has no limit`);
  return limit;
}

function row(id: string) {
  const found = session.getState().groups.flatMap((group) => group.params).find((param) => param.id === id);
  if (found === undefined) throw new Error(`no row ${id}`);
  return found;
}

describe("a started session", () => {
  it("is ready with the default Voron selected and the groups built", () => {
    const state = session.getState();
    expect(state.phase).toBe("ready");
    expect(state.selection).toEqual({
      printer: "Voron 2.4 350 0.4 nozzle",
      process: "0.20mm Standard @Voron",
      filament: "Generic PLA @System",
    });
    expect(state.groups.map((group) => group.id)).toEqual(["speed", "acceleration", "pressure-advance"]);
    expect(state.engine?.name).toBe("ananas-engine");
  });

  it("shows the profile's machine limits when nothing was imported", () => {
    expect(session.getState().resolved?.limits?.source).toBe("profile");
    expect(limitOf("outer_wall_acceleration").source).toBe("printer.max_accel");
  });
});

describe("editing", () => {
  it("applies a value and reports the limit state the engine measures", async () => {
    const limit = limitOf("outer_wall_acceleration").value;
    await session.setValue("outer_wall_acceleration", limit + 1000);
    expect(row("outer_wall_acceleration").value).toBe(limit + 1000);
    const violation = session
      .getState()
      .resolved?.violations.find((v) => v.key === "outer_wall_acceleration" && v.kind === "machine");
    expect(violation?.state).toBe("exceeded");
    expect(violation?.limit).toBe(limit);
  });

  it("refuses an input the engine cannot apply, keeps the old value and marks the row invalid", async () => {
    const before = row("outer_wall_acceleration").value;
    await session.setValue("outer_wall_acceleration", -5);
    expect(row("outer_wall_acceleration").value).toBe(before);
    expect(session.getState().invalid.outer_wall_acceleration).toContain("outside the allowed range");
  });

  it("clears the refusal when the user reverts the row", () => {
    session.dismissInvalid("outer_wall_acceleration");
    expect(session.getState().invalid.outer_wall_acceleration).toBeUndefined();
  });

  it("drops every override on request", async () => {
    await session.resetOverrides();
    expect(session.getState().overrides).toEqual({});
    expect(row("outer_wall_acceleration").value).not.toBe(limitOf("outer_wall_acceleration").value + 1000);
  });
});

describe("imported limits", () => {
  it("replace the profile's limits, name their source and survive an edit", async () => {
    await session.importPrinterCfg(readFileSync(PRINTER_CFG, "utf8"), "printer.cfg");
    const state = session.getState();
    expect(state.imported?.label).toBe("printer.cfg");
    expect(state.resolved?.limits?.source).toBe("printer.cfg");
    expect(limitOf("outer_wall_acceleration").value).toBe(7000);
    expect(limitOf("travel_speed").value).toBe(300);
    await session.setValue("outer_wall_acceleration", 7500);
    expect(limitOf("outer_wall_acceleration").value).toBe(7000);
  });

  it("report a printer.cfg the engine cannot read without losing the earlier import", async () => {
    await session.importPrinterCfg("[printer]\nmax_velocity: x\n", "broken.cfg");
    const state = session.getState();
    expect(state.importError).not.toBeNull();
    expect(state.imported?.label).toBe("printer.cfg");
  });

  it("go back to the profile's limits when cleared", async () => {
    await session.clearImported();
    expect(session.getState().resolved?.limits?.source).toBe("profile");
  });
});

describe("selection", () => {
  it("starts from the chosen process's own values and forgets the overrides", async () => {
    await session.setValue("outer_wall_speed", 111);
    expect(row("outer_wall_speed").value).toBe(111);
    const other = session.getState().compatible?.processes.find((name) => name !== session.getState().selection.process);
    expect(other).toBeDefined();
    await session.selectPreset("process", other!);
    expect(session.getState().overrides).toEqual({});
    expect(row("outer_wall_speed").value).not.toBe(111);
  });

  it("loads another printer with its own compatible presets", async () => {
    const other = session.getState().printers.find((printer) => printer.name !== session.getState().selection.printer);
    expect(other).toBeDefined();
    await session.selectPrinter(other!.name);
    const state = session.getState();
    expect(state.selection.printer).toBe(other!.name);
    expect(state.compatible?.processes).toContain(state.selection.process);
    expect(state.resolved?.printer).toBe(other!.name);
  });
});

describe("slicing", () => {
  it("slices the model with progress and reports the result", async () => {
    await session.selectPrinter("Voron 2.4 350 0.4 nozzle");
    session.setModel(MODEL);
    const percents: number[] = [];
    const stop = session.subscribe(() => {
      const { slice } = session.getState();
      if (slice.status === "running") percents.push(slice.percent);
    });
    const result = await session.slice();
    stop();
    expect(result).not.toBeNull();
    expect(result?.layers).toBe(100);
    expect(existsSync(result!.gcode)).toBe(true);
    expect(session.getState().slice.status).toBe("done");
    expect(percents.length).toBeGreaterThan(5);
    expect([...percents].sort((a, b) => a - b)).toEqual(percents);
  });

  it("fails with the engine's reason for a missing model", async () => {
    session.setModel("/no/such/model.stl");
    const result = await session.slice();
    expect(result).toBeNull();
    expect(session.getState().slice.status).toBe("failed");
    expect(session.getState().slice.error).toContain("model file not found");
  });
});
