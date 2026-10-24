// The printer side of the session. The failure cases need nothing; the rest needs the virtual printer
// (tests/engine/rig/rig.sh up, then ANANAS_RIG_URL=http://127.0.0.1:17125) and is skipped without it.

import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession } from "../src/state/session.ts";
import { MODEL, startEngine } from "./engineHarness.ts";

const RIG = process.env.ANANAS_RIG_URL;
const engine = startEngine();
let session: CockpitSession;

beforeAll(async () => {
  session = new CockpitSession(new EngineClient(engine));
  await session.start();
});

afterAll(async () => {
  await engine.close();
});

const state = () => session.getState();
const row = (id: string) => {
  const found = state().groups.flatMap((group) => group.params).find((param) => param.id === id);
  if (found === undefined) throw new Error(`no row ${id}`);
  return found;
};

async function waitForJob(wanted: string, seconds = 90): Promise<void> {
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await session.refreshPrinter();
    if (state().printer.job?.state === wanted) return;
    await new Promise((resolve) => setTimeout(resolve, 400));
  }
  throw new Error(`the printer did not reach "${wanted}" (last: ${state().printer.job?.state})`);
}

describe("a printer that cannot be used", () => {
  it("starts disconnected", () => {
    expect(state().printer.status).toBe("disconnected");
    expect(state().upload.status).toBe("idle");
  });

  it("says that an unreachable host cannot be reached and stays on the profile's limits", async () => {
    await session.connectPrinter("http://127.0.0.1:1", "");
    expect(state().printer.status).toBe("failed");
    expect(state().printer.error).toContain("cannot reach");
    expect(state().imported).toBeNull();
    expect(state().resolved?.limits?.source).toBe("profile");
  });

  it("refuses an address that is not http or https", async () => {
    await session.connectPrinter("ftp://printer", "");
    expect(state().printer.status).toBe("failed");
    expect(state().printer.error).toContain("http");
  });

  it("refuses to send without a printer or without a slice", async () => {
    await session.sendToPrinter(false);
    expect(state().upload.status).toBe("failed");
    expect(state().upload.error).toMatch(/Slice first|Connect a printer/);
  });
});

describe.skipIf(!RIG)("the virtual printer", () => {
  it("connects and takes over the limits of its running Klipper", async () => {
    await session.connectPrinter(RIG!, "");
    expect(state().printer.status).toBe("connected");
    expect(state().printer.klippyState).toBe("ready");
    expect(state().imported?.label).toBe(RIG);
    expect(state().resolved?.limits?.source).toBe("moonraker");
    expect(row("travel_speed").limit?.value).toBe(300);
    expect(row("outer_wall_acceleration").limit?.value).toBe(7000);
  });

  it("shows at once what the printer's limits do to the profile's speeds", () => {
    // The stock Voron process travels at 350 mm/s, above this printer's max_velocity of 300.
    expect(row("travel_speed").value).toBeGreaterThan(row("travel_speed").limit!.value);
  });

  it("slices, stores the G-code on the printer and does not print it by itself", async () => {
    session.setModel(MODEL);
    await session.loadModel(MODEL);
    const result = await session.slice();
    expect(result).not.toBeNull();
    await session.sendToPrinter(false);
    expect(state().upload.status).toBe("done");
    expect(state().upload.name).toBe("20mm_cube.gcode");
    expect(state().upload.printStarted).toBe(false);
    expect(state().printer.job?.state).not.toBe("printing");
  });

  it("starts the stored file and follows the print to its end", async () => {
    await session.startPrint(state().upload.name!);
    await waitForJob("complete");
    expect(state().printer.job?.filename).toBe("20mm_cube.gcode");
    expect(state().printer.job?.progress).toBeGreaterThan(0.99);
  });

  it("uploads and prints in one go when asked to", async () => {
    await session.sendToPrinter(true);
    expect(state().upload.printStarted).toBe(true);
    await waitForJob("complete");
  });

  it("reports an error of the printer in the upload state", async () => {
    await session.startPrint("not there.gcode");
    expect(state().upload.status).toBe("failed");
    expect(state().upload.error).toMatch(/printer answered|HTTP/);
  });

  it("goes back to the profile's limits when disconnected", async () => {
    await session.disconnectPrinter();
    expect(state().printer.status).toBe("disconnected");
    expect(state().imported).toBeNull();
    expect(state().resolved?.limits?.source).toBe("profile");
  });
});
