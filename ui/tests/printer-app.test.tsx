// The printer dialogs of the app. The connected flow needs the virtual printer (see printer.test.ts).

import { cleanup, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterAll, afterEach, beforeEach, describe, expect, it } from "vitest";
import { App } from "../src/App.tsx";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession } from "../src/state/session.ts";
import { MODEL, startEngine } from "./engineHarness.ts";

const RIG = process.env.ANANAS_RIG_URL;
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

async function openDialog(user: ReturnType<typeof userEvent.setup>) {
  await screen.findByRole("heading", { name: "Kinematics" });
  await user.click(screen.getByRole("button", { name: "Printer connection" }));
  return screen.findByRole("dialog", { name: "Printer" });
}

describe("the printer dialog", () => {
  it("offers to connect, and nothing is connected until the user does", async () => {
    const user = userEvent.setup();
    const dialog = await openDialog(user);
    expect(within(dialog).getByRole("button", { name: "Connect" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "Printer connection" })).toHaveTextContent("Connect printer");
    expect(session.getState().printer.status).toBe("disconnected");
  });

  it("keeps the API key out of sight", async () => {
    const user = userEvent.setup();
    const dialog = await openDialog(user);
    expect(within(dialog).getByLabelText("API key")).toHaveAttribute("type", "password");
  });

  it("shows why a host could not be reached", async () => {
    const user = userEvent.setup();
    const dialog = await openDialog(user);
    await user.type(within(dialog).getByLabelText("Printer address"), "http://127.0.0.1:1");
    await user.click(within(dialog).getByRole("button", { name: "Connect" }));
    expect(await within(dialog).findByTestId("printer-error")).toHaveTextContent(/cannot reach/);
  });

  it("closes with Escape", async () => {
    const user = userEvent.setup();
    await openDialog(user);
    await user.keyboard("{Escape}");
    expect(screen.queryByRole("dialog", { name: "Printer" })).not.toBeInTheDocument();
  });

  it("does not offer to send before a printer is connected", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    await waitFor(() => expect(screen.getByTestId("slice-status")).toHaveAttribute("data-status", "done"), { timeout: 30000 });
    expect(screen.getByRole("button", { name: "Send to printer" })).toBeDisabled();
    expect(screen.getByRole("button", { name: /Send and print/ })).toBeDisabled();
  });
});

describe.skipIf(!RIG)("with the virtual printer", () => {
  it("connects, applies the printer's limits, sends, asks before printing and follows the print", async () => {
    const user = userEvent.setup();
    const dialog = await openDialog(user);
    await user.type(within(dialog).getByLabelText("Printer address"), RIG!);
    await user.click(within(dialog).getByRole("button", { name: "Connect" }));
    expect(await within(dialog).findByTestId("printer-info")).toHaveTextContent(/Klipper ready/);
    await user.click(within(dialog).getByRole("button", { name: "Close" }));

    // The printer's max_velocity (300) is now the limit, and the profile's travel speed (350) is over it.
    await waitFor(() => expect(screen.getByTestId("row-travel_speed")).toHaveAttribute("data-state", "exceeded"));
    expect(screen.getByTestId("machine-line")).toHaveTextContent(RIG!);

    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    // That violation has to be acknowledged before the slice.
    await user.click(within(await screen.findByRole("alertdialog")).getByRole("button", { name: "Slice anyway" }));
    await waitFor(() => expect(screen.getByTestId("slice-status")).toHaveAttribute("data-status", "done"), { timeout: 30000 });

    await user.click(screen.getByRole("button", { name: "Send to printer" }));
    await waitFor(() => expect(screen.getByTestId("upload-status")).toHaveAttribute("data-status", "done"), { timeout: 30000 });
    expect(screen.getByTestId("upload-status")).toHaveTextContent("Stored as 20mm_cube.gcode");
    expect(session.getState().printer.job?.state).not.toBe("printing");

    // Printing is always asked for, naming the printer and the file; cancelling the question prints nothing.
    await user.click(screen.getByRole("button", { name: /Send and print/ }));
    let ask = await screen.findByRole("alertdialog", { name: "Start a print?" });
    expect(ask).toHaveTextContent(RIG!);
    expect(ask).toHaveTextContent("20mm_cube.gcode");
    await user.click(within(ask).getByRole("button", { name: "Cancel" }));
    expect(screen.queryByRole("alertdialog")).not.toBeInTheDocument();
    expect(session.getState().upload.printStarted).toBe(false);

    await user.click(screen.getByRole("button", { name: /Send and print/ }));
    ask = await screen.findByRole("alertdialog", { name: "Start a print?" });
    await user.click(within(ask).getByRole("button", { name: "Upload and print" }));
    await waitFor(() => expect(screen.getByTestId("job")).toHaveAttribute("data-state", "complete"), { timeout: 90000 });
    expect(screen.getByTestId("printer-footer")).toHaveTextContent("complete");
  }, 180000);
});

describe("the native file dialogs", () => {
  it("open a model the user chose and save the G-code where the user chose", async () => {
    cleanup();
    const chosen: string[] = [];
    const platform = {
      native: true,
      pickModel: () => Promise.resolve(MODEL),
      saveGcode: (from: string, name: string) => {
        chosen.push(`${from}|${name}`);
        return Promise.resolve("/Users/test/Desktop/cube.gcode");
      },
    };
    render(<App session={session} platform={platform} />);
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });

    await user.click(screen.getByRole("button", { name: /Choose/ }));
    await waitFor(() => expect(session.getState().model).toBe(MODEL));

    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    await waitFor(() => expect(screen.getByTestId("slice-status")).toHaveAttribute("data-status", "done"), { timeout: 30000 });
    await user.click(screen.getByRole("button", { name: /Save G-code/ }));
    expect(await screen.findByTestId("saved")).toHaveTextContent("Saved to /Users/test/Desktop/cube.gcode");
    expect(chosen[0]).toMatch(/20mm_cube\.gcode\|20mm_cube\.gcode$/);
  });

  it("show a failure of the save and offer no file dialogs in a plain browser", async () => {
    cleanup();
    render(<App session={session} platform={{ native: true, pickModel: () => Promise.resolve(null), saveGcode: () => Promise.reject(new Error("disk full")) }} />);
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    await user.click(screen.getByRole("button", { name: /Slice plate/ }));
    await waitFor(() => expect(screen.getByTestId("slice-status")).toHaveAttribute("data-status", "done"), { timeout: 30000 });
    await user.click(screen.getByRole("button", { name: /Save G-code/ }));
    expect(await screen.findByTestId("saved")).toHaveTextContent("disk full");

    cleanup();
    render(<App session={session} />);
    await screen.findByRole("heading", { name: "Kinematics" });
    expect(screen.queryByRole("button", { name: /Choose/ })).not.toBeInTheDocument();
  });
});
