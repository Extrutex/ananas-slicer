// Das Panel „Kraftverteilung“ in der App (React in jsdom) gegen die echte ananas-engine. Ohne WebGL: Flächen
// werden hier als Box gewählt; das Klicken im Viewport deckt der E2E-Test ab.

import { cleanup, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterAll, afterEach, beforeEach, describe, expect, it } from "vitest";
import { App } from "../src/App.tsx";
import { EngineClient } from "../src/engine/client.ts";
import { CockpitSession } from "../src/state/session.ts";
import { MODEL, startEngine } from "./engineHarness.ts";

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

async function openPanel(user: ReturnType<typeof userEvent.setup>) {
  await screen.findByRole("heading", { name: "Kinematics" });
  await user.click(screen.getByRole("button", { name: "Kraftverteilung" }));
  return screen.findByRole("complementary", { name: "Kraftverteilung" });
}

describe("das Panel Kraftverteilung", () => {
  it("ist zu, bis man es öffnet, und warnt sofort vor den Grenzen des Modells", async () => {
    const user = userEvent.setup();
    await screen.findByRole("heading", { name: "Kinematics" });
    expect(screen.queryByRole("complementary", { name: "Kraftverteilung" })).toBeNull();
    const panel = await openPanel(user);
    const warning = within(panel).getByTestId("stress-panel-warning");
    expect(warning).toHaveTextContent("Infill und Wände sind nicht modelliert");
    expect(warning).toHaveTextContent("Obergrenze der Steifigkeit");
    expect(warning).toHaveTextContent("keine Garantie");
  });

  it("bietet die fünf Werkstoffe als Richtwert an und übernimmt ihre Werte editierbar", async () => {
    const user = userEvent.setup();
    const panel = await openPanel(user);
    const select = within(panel).getByLabelText("Werkstoff") as HTMLSelectElement;
    expect([...select.options].map((option) => option.textContent)).toEqual([
      "PLA (Richtwert)",
      "PETG (Richtwert)",
      "ABS (Richtwert)",
      "ASA (Richtwert)",
      "PA-CF (Richtwert)",
    ]);
    expect(within(panel).getByTestId("material-note")).toHaveTextContent("keine Datenblattwerte");

    await user.selectOptions(select, "pa-cf");
    expect(within(panel).getByLabelText("E-Modul")).toHaveValue("6000");
    expect(within(panel).getByLabelText("z-Faktor")).toHaveValue("0,4");

    // Ein eigener Wert macht aus der Voreinstellung „Eigene Werte“; ein Tippfehler bleibt sichtbar und ändert nichts.
    const yieldField = within(panel).getByLabelText("Streckgrenze");
    await user.clear(yieldField);
    await user.type(yieldField, "8x");
    expect(yieldField).toHaveAttribute("aria-invalid", "true");
    expect(session.getState().stress.setup.material.yieldStrength).toBe(8);
    await user.clear(yieldField);
    await user.type(yieldField, "55,5");
    expect(yieldField).toHaveAttribute("aria-invalid", "false");
    expect(session.getState().stress.setup.material.yieldStrength).toBe(55.5);
    await waitFor(() => expect((within(panel).getByLabelText("Werkstoff") as HTMLSelectElement).value).toBe("custom"));
  });

  it("sperrt den Start, bis Lager und Last da sind, und nennt, was fehlt", async () => {
    const user = userEvent.setup();
    const panel = await openPanel(user);
    const start = within(panel).getByRole("button", { name: "Berechnen" });
    expect(start).toBeDisabled();
    expect(within(panel).getByTestId("stress-errors")).toHaveTextContent("Lagerfläche");
    expect(within(panel).getByTestId("cell-estimate")).toHaveTextContent("Zellen");
  });

  it("rechnet mit Boxen für Lager und Last, zeigt den Fortschritt und das Ergebnis samt Warnung", async () => {
    const user = userEvent.setup();
    const panel = await openPanel(user);
    await user.click(within(panel).getByRole("button", { name: "+ Lagerfläche" }));
    await user.click(within(panel).getByRole("button", { name: "+ Last" }));
    expect(within(panel).getByTestId("pick-hint")).toBeInTheDocument();

    await user.click(within(panel).getByRole("button", { name: "Lager 1 Unterseite" }));
    await user.click(within(panel).getByRole("button", { name: "Last 1 Oberseite" }));
    const force = within(panel).getByLabelText("Last 1 Kraft Z");
    await user.clear(force);
    await user.type(force, "-1000");
    expect(within(panel).getByTestId("region-2-total")).toHaveTextContent("Betrag 1000,0 N");
    expect(within(panel).getByTestId("region-1-place")).toHaveTextContent("Box");

    const start = within(panel).getByRole("button", { name: "Berechnen" });
    await waitFor(() => expect(start).toBeEnabled());
    await user.click(start);

    const summary = await within(panel).findByTestId("stress-summary", {}, { timeout: 30000 });
    expect(session.getState().stress.run.status).toBe("done");
    expect(summary).toHaveTextContent("Max. Spannung");
    expect(summary).toHaveTextContent("1000,0 N / 1000,0 N");
    expect(within(panel).getByRole("button", { name: "Modell zeigen" })).toBeInTheDocument();

    // Die Legende gehört in den Viewport; ohne WebGL zeigt dieser nur den Hinweis, die Legende kommt mit dem Mesh.
    session.setStressDisplay({ quantity: "safety" });
    expect(session.getState().stress.display.quantity).toBe("safety");
  });

  it("markiert das Ergebnis als veraltet, wenn das Objekt bewegt wird, und lässt es dann nicht zeigen", async () => {
    const user = userEvent.setup();
    const panel = await openPanel(user);
    await user.click(within(panel).getByRole("button", { name: "+ Lagerfläche" }));
    await user.click(within(panel).getByRole("button", { name: "+ Last" }));
    await user.click(within(panel).getByRole("button", { name: "Lager 1 Unterseite" }));
    await user.click(within(panel).getByRole("button", { name: "Last 1 Oberseite" }));
    const force = within(panel).getByLabelText("Last 1 Kraft Z");
    await user.clear(force);
    await user.type(force, "-100");
    await user.click(within(panel).getByRole("button", { name: "Berechnen" }));
    await within(panel).findByTestId("stress-summary", {}, { timeout: 30000 });
    expect(within(panel).queryByTestId("stress-stale")).toBeNull();

    await session.setObjectValue("object.position.x", session.getState().objects[0]!.transform.position[0] + 10);
    expect(await within(panel).findByTestId("stress-stale")).toHaveTextContent("Veraltet");
    expect(within(panel).getByRole("button", { name: /Heatmap zeigen|Modell zeigen/ })).toBeDisabled();
  });

  it("schließt per Knopf und mit Escape", async () => {
    const user = userEvent.setup();
    const panel = await openPanel(user);
    await user.click(within(panel).getByRole("button", { name: "Kraftverteilung schließen" }));
    expect(screen.queryByRole("complementary", { name: "Kraftverteilung" })).toBeNull();

    const again = await openPanel(user);
    within(again).getByLabelText("Voxelgröße").focus();
    await user.keyboard("{Escape}");
    expect(screen.queryByRole("complementary", { name: "Kraftverteilung" })).toBeNull();
  });
});
