// The whole cockpit (React in jsdom) against the real ananas-engine.

import { cleanup, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { readFileSync } from "node:fs";
import { afterAll, afterEach, beforeEach, describe, expect, it } from "vitest";
import { App } from "../src/App.tsx";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession } from "../src/state/session.ts";
import { MODEL, PRINTER_CFG, startEngine } from "./engineHarness.ts";

const engine = startEngine();
let session: CockpitSession;

beforeEach(async () => {
  session = new CockpitSession(new EngineClient(engine));
  await session.start();
  session.setModel(MODEL);
  render(<App session={session} />);
});

afterEach(cleanup);

afterAll(async () => {
  await engine.close();
});

const row = (id: string) => screen.getByTestId(`row-${id}`);
const input = (id: string) => within(row(id)).getByRole("textbox");

async function type(user: ReturnType<typeof userEvent.setup>, id: string, text: string) {
  await user.click(input(id));
  await user.keyboard(`{Control>}a{/Control}${text}{Enter}`);
}

describe("the cockpit", () => {
  it("shows the machine, the source of its limits and the rows with their limits", async () => {
    expect(await screen.findByRole("heading", { name: "Kinematics" })).toBeInTheDocument();
    expect(screen.getByTestId("machine-line")).toHaveTextContent("Voron 2.4 350 0.4 nozzle · profile defaults");
    expect(screen.getByTestId("group-speed")).toBeInTheDocument();
    expect(screen.getByTestId("group-acceleration")).toBeInTheDocument();
    expect(row("outer_wall_acceleration")).toHaveAttribute("data-state", expect.stringMatching(/nominal|caution/));
  });

  it("turns a row red, with the limit and Klipper's non-clamping, when a typed value passes the limit", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await type(user, "outer_wall_acceleration", "25000");

    await waitFor(() => expect(row("outer_wall_acceleration")).toHaveAttribute("data-state", "exceeded"));
    expect(within(row("outer_wall_acceleration")).getByText("LIMIT")).toBeInTheDocument();
    expect(within(row("outer_wall_acceleration")).getByText(/Klipper does not clamp this value/)).toBeInTheDocument();
    expect(screen.getByTestId("status-line")).toHaveTextContent(/exceeds a machine limit/);
  });

  it("replaces the value when the user types right after focusing the field", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await user.click(input("outer_wall_acceleration"));
    await user.keyboard("4000{Enter}");
    await waitFor(() => expect(session.getState().overrides.outer_wall_acceleration).toBe("4000"));
  });

  it("keeps the violation count visible when the group is collapsed", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await type(user, "outer_wall_acceleration", "25000");
    await waitFor(() => expect(row("outer_wall_acceleration")).toHaveAttribute("data-state", "exceeded"));

    const header = within(screen.getByTestId("group-acceleration")).getByRole("button", { name: /Acceleration/ });
    await user.click(header);
    expect(header).toHaveAttribute("aria-expanded", "false");
    expect(within(screen.getByTestId("group-acceleration")).getByText("1 OVER")).toBeInTheDocument();
  });

  it("marks an input that is not a number invalid and does not send it to the engine", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    const before = session.getState().resolved;
    await user.click(input("outer_wall_acceleration"));
    await user.keyboard("{Control>}a{/Control}abc");

    expect(row("outer_wall_acceleration")).toHaveAttribute("data-state", "invalid");
    expect(within(row("outer_wall_acceleration")).getByText("Not a number.")).toBeInTheDocument();
    await user.keyboard("{Enter}");
    expect(session.getState().resolved).toBe(before);
  });

  it("steps a value with the arrow keys and ten times with Shift", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    const start = Number(input("outer_wall_acceleration").getAttribute("value")?.replace(/\D/g, ""));
    await user.click(input("outer_wall_acceleration"));
    await user.keyboard("{ArrowUp}");
    await waitFor(() => expect(session.getState().overrides.outer_wall_acceleration).toBe(String(start + 100)));
    await user.keyboard("{Shift>}{ArrowDown}{/Shift}");
    await waitFor(() => expect(session.getState().overrides.outer_wall_acceleration).toBe(String(start - 900)));
  });

  it("takes the limits of an imported printer.cfg and names the file", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    const file = new File([readFileSync(PRINTER_CFG, "utf8")], "printer.cfg", { type: "text/plain" });
    await user.upload(screen.getByTestId("printer-cfg-input"), file);

    await waitFor(() => expect(screen.getByTestId("machine-line")).toHaveTextContent("printer.cfg · printer.cfg"));
    expect(within(row("travel_speed")).getByTitle("printer.max_velocity = 300 mm/s")).toBeInTheDocument();
    expect(within(row("outer_wall_acceleration")).getByTitle(/^printer\.max_accel = 7.000 mm/)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Use profile limits" }));
    await waitFor(() => expect(screen.getByTestId("machine-line")).toHaveTextContent("profile defaults"));
  });

  it("asks for an acknowledgement that lists the violation before it slices past a limit", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await type(user, "outer_wall_acceleration", "25000");
    await waitFor(() => expect(row("outer_wall_acceleration")).toHaveAttribute("data-state", "exceeded"));

    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    const dialog = await screen.findByRole("alertdialog");
    expect(within(dialog).getByText(/1 limit violation open/)).toBeInTheDocument();
    expect(within(dialog).getByText("printer.max_accel")).toBeInTheDocument();

    await user.click(within(dialog).getByRole("button", { name: "Cancel" }));
    expect(screen.queryByRole("alertdialog")).not.toBeInTheDocument();
    expect(session.getState().slice.status).toBe("idle");

    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    await user.click(within(await screen.findByRole("alertdialog")).getByRole("button", { name: "Slice anyway" }));
    const status = await screen.findByTestId("slice-status");
    await waitFor(() => expect(status).toHaveAttribute("data-status", "done"), { timeout: 30000 });
    expect(screen.getByTestId("slice-summary")).toHaveTextContent("100");
  });

  it("slices straight away when nothing exceeds a limit", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    expect(screen.queryByRole("alertdialog")).not.toBeInTheDocument();
    await waitFor(() => expect(screen.getByTestId("slice-status")).toHaveAttribute("data-status", "done"), { timeout: 30000 });
  });

  it("loads another printer from the selector", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    const other = session.getState().printers.find((printer) => printer.name !== session.getState().selection.printer)!;
    await user.selectOptions(screen.getByRole("combobox", { name: "Printer" }), other.name);
    await waitFor(() => expect(screen.getByTestId("machine-line")).toHaveTextContent(other.name));
  });
});
