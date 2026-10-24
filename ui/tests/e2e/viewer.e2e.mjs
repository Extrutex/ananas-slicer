// End-to-end check of the viewer in a real browser: starts the dev bridge and Vite on their own ports, drives
// headless Chrome over the DevTools protocol (software WebGL), and checks both the page and the pixels the
// scene draws. Not part of `npm test`: run it with `npm run e2e`. Set CHROME_BIN to choose the browser;
// without one the check is skipped. Screenshots go to tests/e2e/out/.

import { spawn } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ui = resolve(import.meta.dirname, "../..");
const repo = resolve(ui, "..");
const out = join(import.meta.dirname, "out");
const BRIDGE_PORT = 4273;
const VITE_PORT = 1520;
const DEBUG_PORT = 9533;

const chromePath = [process.env.CHROME_BIN, "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/usr/bin/google-chrome", "/usr/bin/chromium"].find((p) => p && existsSync(p));
if (!chromePath) {
  console.log("skipped: no Chrome found (set CHROME_BIN)");
  process.exit(0);
}
mkdirSync(out, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const children = [];
const spawnChild = (command, args, options = {}) => {
  const child = spawn(command, args, { stdio: "ignore", cwd: ui, ...options });
  children.push(child);
  return child;
};
let failures = 0;
const check = (name, ok, detail = "") => {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? ` (${detail})` : ""}`);
  if (!ok) failures += 1;
};

async function main() {
  spawnChild("node", ["dev-bridge.ts"], { env: { ...process.env, ANANAS_BRIDGE_PORT: String(BRIDGE_PORT) } });
  spawnChild("npx", ["vite", "--host", "127.0.0.1", "--port", String(VITE_PORT), "--strictPort"], {
    env: { ...process.env, VITE_BRIDGE_URL: `http://127.0.0.1:${BRIDGE_PORT}` },
  });
  for (let i = 0; i < 100; i += 1) {
    try {
      await fetch(`http://127.0.0.1:${BRIDGE_PORT}/defaults`);
      await fetch(`http://127.0.0.1:${VITE_PORT}/`);
      break;
    } catch {
      await sleep(200);
    }
  }

  const profile = mkdtempSync(join(tmpdir(), "ananas-e2e-"));
  spawnChild(chromePath, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist", "--enable-webgl",
    `--remote-debugging-port=${DEBUG_PORT}`, `--user-data-dir=${profile}`, "--window-size=1600,950", "about:blank",
  ]);
  let targets = [];
  for (let i = 0; i < 100 && !targets.some((t) => t.type === "page"); i += 1) {
    try {
      targets = await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json`)).json();
    } catch {
      await sleep(200);
    }
  }
  const ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const waiting = new Map();
  const problems = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) {
      waiting.get(d.id)(d);
      waiting.delete(d.id);
    } else if (d.method === "Runtime.consoleAPICalled" && d.params.type === "error") {
      problems.push(d.params.args.map((a) => a.value ?? a.description).join(" ").slice(0, 200));
    } else if (d.method === "Runtime.exceptionThrown") {
      problems.push(`exception: ${(d.params.exceptionDetails.exception?.description ?? d.params.exceptionDetails.text).slice(0, 200)}`);
    }
  };
  const send = (method, params = {}) => new Promise((r) => { const n = ++id; waiting.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const evaluate = async (expression) => (await send("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true })).result?.result?.value;
  const shot = async (name) => writeFileSync(join(out, `${name}.png`), Buffer.from((await send("Page.captureScreenshot", { format: "png" })).result.data, "base64"));
  const key = (k, code, vk) => send("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk }).then(() => send("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk }));
  const waitFor = async (expression, tries = 120) => { for (let i = 0; i < tries; i += 1) { if (await evaluate(expression)) return true; await sleep(250); } return false; };
  const click = (text) => evaluate(`[...document.querySelectorAll('button')].find((b) => b.textContent.trim() === ${JSON.stringify(text)} || b.textContent.startsWith(${JSON.stringify(text)}))?.click()`);
  const clickIn = (scope, text) => evaluate(`[...document.querySelectorAll(${JSON.stringify(scope + " button")})].find((b) => b.textContent.trim() === ${JSON.stringify(text)})?.click()`);
  const setInput = (selector, value) => evaluate(`(() => { const el = document.querySelector(${JSON.stringify(selector)}); Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, ${JSON.stringify(String(value))}); el.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  const typeInto = async (rowId, text) => { await evaluate(`document.querySelector('[data-testid="row-${rowId}"] input').focus()`); await sleep(150); await send("Input.insertText", { text }); await key("Enter", "Enter", 13); await sleep(900); };
  const open = async (path) => { await setInput('input[aria-label="Model file"]', path); await click("Open"); await sleep(3500); };
  // What the scene draws: the share of bright pixels, the number of distinct colours and a signature of the picture.
  const pixels = () => evaluate(`(() => { const c = document.querySelector('[data-testid="viewer-canvas"]'); const t = document.createElement('canvas'); t.width = c.width; t.height = c.height; const x = t.getContext('2d'); x.drawImage(c, 0, 0); const d = x.getImageData(0, 0, t.width, t.height).data; const colours = new Set(); let bright = 0, sum = 0; for (let i = 0; i < d.length; i += 4) { const l = (d[i] + d[i + 1] + d[i + 2]) / 3; sum += l; if (l > 90) bright += 1; colours.add((d[i] >> 3) << 10 | (d[i + 1] >> 3) << 5 | d[i + 2] >> 3); } return { bright: bright / (d.length / 4), colours: colours.size, mean: sum / (d.length / 4) }; })()`);
  const differs = (a, b) => Math.abs(a.mean - b.mean) > 0.4 || Math.abs(a.bright - b.bright) > 0.002;

  await send("Page.enable");
  await send("Runtime.enable");
  await send("Emulation.setDeviceMetricsOverride", { width: 1600, height: 950, deviceScaleFactor: 1, mobile: false });
  await send("Page.navigate", { url: `http://127.0.0.1:${VITE_PORT}/` });

  // --- the model on the bed ---
  check("the viewer draws", await waitFor(`Number(document.querySelector('[data-testid="viewer-canvas"]')?.dataset.drawn) > 0 && !!document.querySelector('[data-testid="mesh-stats"]')`));
  await sleep(2500);
  await shot("1-solid");
  const solid = await pixels();
  check("the scene is not empty or flat", solid.colours > 40 && solid.mean > 8, `${solid.colours} colours, mean ${solid.mean.toFixed(1)}`);
  check("the model is lit", solid.bright > 0.0003 && solid.bright < 0.5, `${(solid.bright * 100).toFixed(2)} % bright`);

  await click("Mesh"); await sleep(1500);
  await shot("2-mesh");
  const mesh = await pixels();
  check("the mesh view looks different from the solid view", differs(solid, mesh));

  await click("Inspect"); await sleep(1500);
  const shares = await evaluate(`[...document.querySelectorAll('[data-testid="inspect-legend"] li span:last-child')].map((s) => parseFloat(s.textContent))`);
  check("the inspection legend adds up to the whole surface", shares?.length === 4 && Math.abs(shares.reduce((a, b) => a + b, 0) - 100) < 1.5, JSON.stringify(shares));
  await shot("3-inspect");

  await click("Solid");
  await typeInto("object.rotation.x", "45");
  const size = await evaluate(`[...document.querySelectorAll('[data-testid^="row-object.size"] input')].map((i) => i.value)`);
  check("a typed rotation changes the size the engine reports", size?.[1] === "28.28" && size?.[2] === "28.28", JSON.stringify(size));
  await typeInto("object.rotation.x", "0");

  await typeInto("object.position.x", "900");
  check("an object off the bed is flagged and cannot be sliced", (await evaluate(`document.querySelector('[data-testid="mesh-stats"]').innerText.includes('outside the print area') && [...document.querySelectorAll('button')].find((b) => b.textContent.startsWith('Slice plate')).disabled`)) === true);
  await shot("4-off-bed");
  await typeInto("object.position.x", "175");

  // --- a model in many parts ---
  await open(`${repo}/resources/handy_models/OrcaSliced.3mf`);
  check("a multi-part model loads", (await evaluate(`document.querySelector('[data-testid="mesh-stats"]')?.innerText`))?.includes("143"), "143 912 triangles");
  await sleep(1500);
  const together = await pixels();
  await setInput('input[aria-label="Explode"]', 80); await sleep(1800);
  await shot("5-exploded");
  const apart = await pixels();
  check("the explosion slider pulls the parts apart", differs(together, apart));

  // --- the toolpaths ---
  await open(`${repo}/tests/data/20mm_cube.obj`);
  await click("Slice plate");
  check("slicing shows the heatmap legend", await waitFor(`!!document.querySelector('[data-testid="heatmap-legend"]')`, 200));
  await sleep(3500);
  await shot("6-gcode-speed");
  const speed = await pixels();
  check("the toolpaths are drawn", speed.colours > 40 && speed.bright > 0.0003, `${speed.colours} colours`);
  await click("Accel"); await sleep(2500);
  await shot("7-gcode-accel");
  check("another quantity recolours the paths", differs(speed, await pixels()));
  const accel = await pixels();
  await setInput('input[aria-label="Layer to"]', 30); await sleep(2500);
  await shot("8-gcode-layers");
  check("the layer range hides the upper layers", differs(accel, await pixels()));
  const legend = await evaluate(`document.querySelector('[data-testid="heatmap-legend"]').innerText.replace(/\\n/g, ' | ')`);
  check("the legend names the limit", /Over limit/.test(legend), legend);

  // --- Kraftverteilung: Flächen per Klick im Viewport wählen, rechnen, Heatmap ---
  await click("Model"); await sleep(800);
  await click("Kraftverteilung"); await sleep(500);
  check("das Panel Kraftverteilung öffnet sich mit dem Warnhinweis", await waitFor(`!!document.querySelector('[data-testid="stress-panel"]') && document.querySelector('[data-testid="stress-panel-warning"]').innerText.includes('Obergrenze der Steifigkeit')`, 20));
  const centre = await evaluate(`(() => { const r = document.querySelector('[data-testid="viewer-canvas"]').getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
  const clickAt = async ({ x, y }) => {
    await send("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", clickCount: 1 });
    await send("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", clickCount: 1 });
    await sleep(500);
  };
  await click("+ Lagerfläche");
  await click("Under"); await sleep(1200);
  await clickAt(centre);
  check("ein Klick auf die Unterseite wählt eine Lagerfläche (2 Dreiecke)", (await evaluate(`document.querySelector('[data-testid="region-1-place"]')?.innerText`)) === "2 Dreiecke");
  await shot("12-stress-support");
  await click("+ Last");
  await click("Top"); await sleep(1200);
  await clickAt(centre);
  check("ein Klick auf die Oberseite wählt die Lastfläche (2 Dreiecke)", (await evaluate(`document.querySelector('[data-testid="region-2-place"]')?.innerText`)) === "2 Dreiecke");
  await setInput('input[aria-label="Last 1 Kraft Z"]', "-1000"); await sleep(300);
  await shot("13-stress-load");
  const marked = await pixels();
  await click("Iso"); await sleep(800);
  check("Berechnen ist freigegeben", (await evaluate(`[...document.querySelectorAll('[data-testid="stress-panel"] button')].find((b) => b.textContent.trim() === 'Berechnen')?.disabled`)) === false);
  await clickIn('[data-testid="stress-panel"]', "Berechnen");
  check("die Heatmap erscheint mit Legende", await waitFor(`!!document.querySelector('[data-testid="stress-legend"]')`, 120));
  await sleep(1500);
  await shot("14-stress-vonmises");
  const vm = await pixels();
  check("die Heatmap ist gezeichnet", vm.colours > 40 && differs(marked, vm), `${vm.colours} Farben`);
  const legendText = await evaluate(`document.querySelector('[data-testid="stress-legend"]').innerText.replace(/\\n/g, ' | ')`);
  check("die Legende nennt Maxima und den Hinweis zu Infill und Wänden", /Max\. Spannung/.test(legendText) && /Min\. Sicherheit/.test(legendText) && /Infill und Wände sind nicht modelliert/.test(legendText) && /Richtwerte, keine Garantie/.test(legendText), legendText.slice(0, 160));
  await clickIn('[data-testid="stress-legend"]', "Sicherheit"); await sleep(1500);
  await shot("15-stress-safety");
  const safety = await pixels();
  check("Sicherheit färbt anders als Spannung", differs(vm, safety));
  await clickIn('[data-testid="stress-legend"]', "Verformung"); await sleep(1500);
  await shot("16-stress-displacement");
  check("Verformung färbt anders als Sicherheit", differs(safety, await pixels()));
  await setInput('input[aria-label="Schnitt durch das Teil"]', 4); await sleep(1500);
  await shot("17-stress-cut");
  check("der Schnitt legt das Innere frei", differs(await pixels(), safety));

  // --- the way to the printer: only against the virtual printer (tests/engine/rig/rig.sh up) ---
  const rig = process.env.ANANAS_RIG_URL;
  if (rig) {
    await evaluate(`document.querySelector('button[aria-label="Printer connection"]').click()`);
    await sleep(500);
    await setInput('input[aria-label="Printer address"]', rig);
    await clickIn("[role=dialog]", "Connect");
    check("the app connects to the printer", await waitFor(`!!document.querySelector('[data-testid="printer-info"]')`, 40));
    await shot("9-printer-dialog");
    await clickIn("[role=dialog]", "Close"); await sleep(1200);
    check("the printer's limits replace the profile's", (await evaluate(`document.querySelector('[data-testid="machine-line"]').innerText`)).includes(rig));
    await click("Model"); await sleep(300);
    await click("Slice plate");
    if (await waitFor(`!!document.querySelector('[role=alertdialog]')`, 10)) await evaluate(`[...document.querySelectorAll('[role=alertdialog] button')].find((b) => b.textContent === 'Slice anyway').click()`);
    check("the slice finishes", await waitFor(`document.querySelector('[data-testid="slice-status"]')?.dataset.status === 'done'`, 160));
    await click("Send and print");
    check("starting a print is asked for first", await waitFor(`document.querySelector('[role=alertdialog]')?.innerText.includes('Start a print?')`, 20));
    await shot("10-print-dialog");
    check("nothing prints before the answer", (await evaluate(`document.querySelector('[data-testid="job"]')?.dataset.state ?? 'none'`)) !== "printing");
    await clickIn("[role=alertdialog]", "Upload and print");
    check("the print runs to its end", await waitFor(`document.querySelector('[data-testid="job"]')?.dataset.state === 'complete'`, 240));
    await shot("11-job-complete");
  }

  check("the console stays clean", problems.length === 0, problems.slice(0, 3).join(" ; "));
  ws.close();
  rmSync(profile, { recursive: true, force: true });
}

try {
  await main();
} catch (error) {
  console.log(`FAIL e2e crashed: ${error}`);
  failures += 1;
} finally {
  for (const child of children) child.kill();
}
console.log(failures === 0 ? "PASS" : `${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
