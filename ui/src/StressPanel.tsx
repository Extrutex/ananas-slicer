import { MATERIAL_PRESETS, CUSTOM_MATERIAL_ID, presetIdOf } from "./stress/materials.ts";
import { NumberField } from "./stress/NumberField.tsx";
import { INFILL_WARNING } from "./stress/texts.ts";
import { checkSetup, hasPlace, regionName, type StressRegion, type Vec3 } from "./stress/setup.ts";
import { formatValue } from "./stress/colors.ts";
import { stressResultCurrent, type CockpitSession, type CockpitState } from "./state/session.ts";

const BUTTON = "h-target rounded border px-2 text-ks-table";
const NEUTRAL = `${BUTTON} border-edge-strong bg-surface-raised text-ink hover:bg-surface-hover`;
const ACTIVE = `${BUTTON} border-edge-strong bg-carbon-700 font-semibold text-ink-strong`;
const SELECT = "h-target w-full rounded border border-edge-field bg-surface-field px-1.5 font-data text-ks-table text-ink-strong";
const AXES = ["x", "y", "z"] as const;

const de = (value: number, digits = 2): string => value.toFixed(digits).replace(".", ",");

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="border-t border-edge px-3 py-2">
      <h3 className="m-0 mb-1.5 text-ks-table font-semibold text-ink-strong">{title}</h3>
      {children}
    </section>
  );
}

function RegionEditor({ region, session, state }: { region: StressRegion; session: CockpitSession; state: CockpitState }) {
  const { stress } = state;
  const name = regionName(region, stress.setup.regions);
  const armed = stress.armed === region.id;
  const object = state.objects[stress.object];
  const box = region.box;

  const setBox = (axisIndex: number, end: "min" | "max", value: number) => {
    const base = box ?? { min: [...(object?.box.min ?? [0, 0, 0])] as Vec3, max: [...(object?.box.max ?? [0, 0, 0])] as Vec3 };
    const next = { min: [...base.min] as Vec3, max: [...base.max] as Vec3 };
    next[end][axisIndex] = value;
    session.updateStressRegion(region.id, { box: next });
  };
  // Eine Lage der Box am Objekt: die Unterseite bzw. die Oberseite (eine Schicht Knoten, die Engine lässt eine halbe Zelle Spiel).
  const side = (which: "bottom" | "top") => {
    if (!object) return;
    const z = which === "bottom" ? object.box.min[2] : object.box.max[2];
    session.updateStressRegion(region.id, { box: { min: [object.box.min[0], object.box.min[1], z], max: [object.box.max[0], object.box.max[1], z] } });
  };

  return (
    <li className={`border p-2 ${armed ? "border-edge-strong bg-surface-raised" : "border-edge-subtle"}`} data-testid={`region-${region.id}`} data-kind={region.kind}>
      <div className="flex items-center gap-2">
        <span
          aria-hidden="true"
          className="inline-block h-2.5 w-2.5 rounded-[1px]"
          style={{ background: region.kind === "support" ? "#4db9ff" : "#ffb23e" }}
        />
        <span className="font-semibold text-ink-strong">{name}</span>
        <span className="ml-auto text-ks-micro text-ink-muted" data-testid={`region-${region.id}-place`}>
          {hasPlace(region) ? [region.triangles.length > 0 ? `${region.triangles.length} Dreiecke` : null, box ? "Box" : null].filter(Boolean).join(" · ") : "nichts gewählt"}
        </span>
        <button type="button" aria-label={`${name} entfernen`} onClick={() => session.removeStressRegion(region.id)} className={`${BUTTON} border-edge text-ink-secondary hover:bg-surface-hover`}>
          ✕
        </button>
      </div>

      <div className="mt-1.5 flex flex-wrap gap-1.5">
        <button
          type="button"
          aria-pressed={armed}
          aria-label={`${name} im Viewport wählen`}
          onClick={() => session.armStressRegion(armed ? null : region.id)}
          className={armed ? ACTIVE : NEUTRAL}
        >
          {armed ? "Wählen beenden" : "Im Viewport wählen"}
        </button>
        <button type="button" aria-label={`${name} Unterseite`} onClick={() => side("bottom")} className={NEUTRAL}>
          Unterseite
        </button>
        <button type="button" aria-label={`${name} Oberseite`} onClick={() => side("top")} className={NEUTRAL}>
          Oberseite
        </button>
        {region.triangles.length > 0 || box !== null ? (
          <button type="button" aria-label={`${name} leeren`} onClick={() => session.updateStressRegion(region.id, { triangles: [], box: null })} className={NEUTRAL}>
            Leeren
          </button>
        ) : null}
      </div>

      <details className="mt-1.5" open={box !== null}>
        <summary className="cursor-pointer text-ks-micro text-ink-muted">Box (mm, Bettkoordinaten)</summary>
        <div className="mt-1 grid grid-cols-3 gap-1.5">
          {AXES.map((axis, a) => (
            <NumberField key={`min-${axis}`} label={`${name} Box min ${axis}`} value={box?.min[a] ?? object?.box.min[a] ?? 0} onChange={(v) => setBox(a, "min", v)} />
          ))}
          {AXES.map((axis, a) => (
            <NumberField key={`max-${axis}`} label={`${name} Box max ${axis}`} value={box?.max[a] ?? object?.box.max[a] ?? 0} onChange={(v) => setBox(a, "max", v)} />
          ))}
        </div>
        <p className="m-0 mt-1 text-ks-micro text-ink-muted">Die Box nimmt alle Knoten darin, mit einer halben Zelle Spiel. Eine Fläche ohne Dicke (min = max) genügt.</p>
      </details>

      {region.kind === "support" ? (
        <div className="mt-1.5 flex items-center gap-3 text-ks-micro text-ink-muted">
          <span>Festgehalten:</span>
          {AXES.map((axis, a) => (
            <label key={axis} className="flex items-center gap-1">
              <input
                type="checkbox"
                aria-label={`${name} hält ${axis}`}
                checked={region.axes[a]}
                onChange={(event) => {
                  const axes = [...region.axes] as [boolean, boolean, boolean];
                  axes[a] = event.target.checked;
                  session.updateStressRegion(region.id, { axes });
                }}
              />
              {axis}
            </label>
          ))}
        </div>
      ) : (
        <div className="mt-1.5">
          <div className="grid grid-cols-3 gap-1.5">
            {AXES.map((axis, a) => (
              <NumberField
                key={axis}
                label={`${name} Kraft ${axis.toUpperCase()}`}
                unit="N"
                value={region.force[a]!}
                onChange={(v) => {
                  const force = [...region.force] as Vec3;
                  force[a] = v;
                  session.updateStressRegion(region.id, { force });
                }}
              />
            ))}
          </div>
          <p className="m-0 mt-1 text-ks-micro text-ink-muted" data-testid={`region-${region.id}-total`}>
            Betrag {de(Math.hypot(...region.force), 1)} N {"·"} Richtung der Kraft auf das Teil (z nach oben, −z drückt auf das Bett)
          </p>
        </div>
      )}
    </li>
  );
}

export function StressPanel({ session, state }: { session: CockpitSession; state: CockpitState }) {
  const { stress } = state;
  if (!stress.open) return null;
  const { setup, run, result } = stress;
  const object = state.objects[stress.object];
  const check = checkSetup(setup, object?.box ?? null);
  const running = run.status === "running";
  const current = stressResultCurrent(state);
  const stale = result !== null && !current;
  const presetId = presetIdOf(setup.material);
  const summary = result?.summary;
  const disabled = running || check.errors.length > 0 || object === undefined;

  return (
    <aside
      aria-label="Kraftverteilung"
      className="absolute right-3 top-[60px] z-[5] flex max-h-[calc(100%-72px)] w-[380px] flex-col overflow-hidden border border-edge-strong bg-surface-panel/95 font-ui text-ks-table text-ink"
      data-testid="stress-panel"
      onKeyDown={(event) => event.key === "Escape" && !running && session.closeStress()}
    >
      <header className="flex items-center border-b border-edge px-3 py-2">
        <h2 className="m-0 text-ks-section font-semibold text-ink-strong">Kraftverteilung</h2>
        <button type="button" aria-label="Kraftverteilung schließen" onClick={() => session.closeStress()} className={`${BUTTON} ml-auto border-edge text-ink-secondary hover:bg-surface-hover`}>
          Schließen
        </button>
      </header>

      <div className="min-h-0 flex-1 overflow-y-auto">
        <p className="m-0 px-3 py-2 text-ks-micro text-signal-caution" data-testid="stress-panel-warning">
          {INFILL_WARNING}
        </p>

        {state.objects.length > 1 ? (
          <Section title="Objekt">
            <select aria-label="Objekt der Kraftverteilung" className={SELECT} value={stress.object} onChange={(event) => session.setStressObject(Number(event.target.value))}>
              {state.objects.map((candidate, index) => (
                <option key={candidate.id} value={index}>
                  {candidate.name || `Objekt ${index + 1}`}
                </option>
              ))}
            </select>
          </Section>
        ) : null}

        <Section title="Werkstoff">
          <label className="flex flex-col gap-0.5 text-ks-micro text-ink-muted">
            Voreinstellung
            <select
              aria-label="Werkstoff"
              className={SELECT}
              value={presetId}
              onChange={(event) => {
                const preset = MATERIAL_PRESETS.find((candidate) => candidate.id === event.target.value);
                if (preset) session.setStressMaterial({ youngsModulus: preset.youngsModulus, yieldStrength: preset.yieldStrength, poissonRatio: preset.poissonRatio, zFactor: preset.zFactor });
              }}
            >
              {MATERIAL_PRESETS.map((preset) => (
                <option key={preset.id} value={preset.id}>
                  {preset.label} (Richtwert)
                </option>
              ))}
              {presetId === CUSTOM_MATERIAL_ID ? <option value={CUSTOM_MATERIAL_ID}>Eigene Werte</option> : null}
            </select>
          </label>
          <p className="m-0 mt-1 text-ks-micro text-ink-muted" data-testid="material-note">
            Richtwerte für gedruckte FDM-Teile bei Belastung in der Druckebene, keine Datenblattwerte. Hersteller, Temperatur, Wände und Trocknung verschieben sie; für ein belastbares Ergebnis eigene Messwerte eintragen.
          </p>
          <div className="mt-1.5 grid grid-cols-2 gap-1.5">
            <NumberField label="E-Modul" unit="MPa" value={setup.material.youngsModulus} onChange={(v) => session.setStressMaterial({ youngsModulus: v })} />
            <NumberField label="Streckgrenze" unit="MPa" value={setup.material.yieldStrength} onChange={(v) => session.setStressMaterial({ yieldStrength: v })} />
            <NumberField label="Querkontraktion" value={setup.material.poissonRatio} onChange={(v) => session.setStressMaterial({ poissonRatio: v })} />
            <NumberField label="z-Faktor" value={setup.material.zFactor} onChange={(v) => session.setStressMaterial({ zFactor: v })} />
          </div>
          <p className="m-0 mt-1 text-ks-micro text-ink-muted">
            z-Faktor: Anteil von Steifigkeit und Zugfestigkeit senkrecht zu den Schichten (Schichtverbund), 0,05 bis 1. Ein Teil bricht meist zuerst zwischen den Schichten.
          </p>
        </Section>

        <Section title="Gitter">
          <NumberField label="Voxelgröße" unit="mm" value={setup.voxelSize} onChange={(v) => session.setStressVoxelSize(v)} />
          <p className="m-0 mt-1 text-ks-micro text-ink-muted" data-testid="cell-estimate">
            {check.cells > 0 ? `Etwa ${check.cells.toLocaleString("de-DE")} Zellen. ` : ""}Feiner rechnet genauer, aber länger; an Ecken und Lastpunkten steigt die Spannung mit feinerem Gitter weiter.
          </p>
        </Section>

        <Section title="Lagerflächen und Lasten">
          <ul className="m-0 flex list-none flex-col gap-2 p-0">
            {setup.regions.map((region) => (
              <RegionEditor key={region.id} region={region} session={session} state={state} />
            ))}
          </ul>
          <div className="mt-2 flex gap-1.5">
            <button type="button" onClick={() => session.addStressRegion("support")} className={NEUTRAL}>
              + Lagerfläche
            </button>
            <button type="button" onClick={() => session.addStressRegion("load")} className={NEUTRAL}>
              + Last
            </button>
          </div>
          {stress.armed !== null ? (
            <p className="m-0 mt-1.5 text-ks-micro text-ink-secondary" data-testid="pick-hint">
              Klicken Sie im Viewport auf eine Fläche, um sie zu wählen; ein zweiter Klick nimmt sie wieder heraus. {stress.pickNote ?? ""}
            </p>
          ) : null}
        </Section>

        <Section title="Rechnung">
          {check.errors.length > 0 ? (
            <ul className="m-0 list-disc pl-4 text-ks-micro text-signal-caution" data-testid="stress-errors">
              {check.errors.map((error) => (
                <li key={error}>{error}</li>
              ))}
            </ul>
          ) : null}
          {check.hints.length > 0 ? (
            <ul className="m-0 mt-1 list-disc pl-4 text-ks-micro text-ink-muted" data-testid="stress-hints">
              {check.hints.map((hint) => (
                <li key={hint}>{hint}</li>
              ))}
            </ul>
          ) : null}
          <div className="mt-2 flex items-center gap-2">
            <button type="button" disabled={disabled} onClick={() => void session.runStress()} className={`${ACTIVE} disabled:cursor-not-allowed disabled:text-ink-disabled`}>
              {running ? "Rechnet …" : "Berechnen"}
            </button>
            <span className="text-ks-micro text-ink-muted">Die Rechnung lässt sich nicht abbrechen.</span>
          </div>
          {run.status !== "idle" ? (
            <div className="mt-2" data-testid="stress-run" data-status={run.status}>
              {running ? (
                <div>
                  <div role="progressbar" aria-label="Fortschritt der Kraftverteilung" aria-valuemin={0} aria-valuemax={100} aria-valuenow={run.percent} className="h-1.5 w-full bg-surface-field">
                    <div className="h-full bg-signal-path" style={{ width: `${run.percent}%` }} />
                  </div>
                  <p className="m-0 mt-1 truncate font-data text-ks-table text-ink-muted">
                    {run.percent} % {"·"} {run.message}
                  </p>
                </div>
              ) : null}
              {run.status === "failed" ? (
                <p role="alert" className="m-0 font-data text-ks-table text-signal-limit" data-testid="stress-error">
                  {run.error}
                </p>
              ) : null}
            </div>
          ) : null}
        </Section>

        {result && summary ? (
          <Section title="Ergebnis">
            {stale ? (
              <p role="alert" className="m-0 mb-1.5 text-signal-caution" data-testid="stress-stale">
                Veraltet: Das Objekt oder das Modell hat sich seit der Rechnung geändert. Neu berechnen.
              </p>
            ) : null}
            <dl className="m-0 grid grid-cols-[auto_1fr] gap-x-4 gap-y-0.5 font-data" data-testid="stress-summary">
              <dt className="text-ink-muted">Max. Spannung</dt>
              <dd className="m-0 text-ink-strong">{formatValue("von-mises", summary.max_von_mises.value)} MPa</dd>
              <dt className="text-ink-muted">Min. Sicherheit</dt>
              <dd className={`m-0 ${summary.min_safety.value < 1 ? "text-signal-limit" : "text-ink-strong"}`}>{formatValue("safety", summary.min_safety.value)}</dd>
              <dt className="text-ink-muted">Max. Verformung</dt>
              <dd className="m-0 text-ink-strong">{formatValue("displacement", summary.max_displacement.value)} mm</dd>
              <dt className="text-ink-muted">Last / Reaktion</dt>
              <dd className="m-0 text-ink-strong">
                {de(Math.hypot(...summary.applied_force), 1)} N / {de(Math.hypot(...summary.reaction), 1)} N
              </dd>
              <dt className="text-ink-muted">Zellen</dt>
              <dd className="m-0 text-ink-strong">
                {summary.solid_cells.toLocaleString("de-DE")} {"·"} {summary.solver.iterations} Iterationen
              </dd>
            </dl>
            {summary.warnings.length > 0 ? (
              <ul className="m-0 mt-1.5 list-disc pl-4 text-ks-micro text-signal-caution" data-testid="stress-warnings">
                {summary.warnings.map((warning) => (
                  <li key={warning}>{warning}</li>
                ))}
              </ul>
            ) : null}
            <div className="mt-2 flex gap-1.5">
              <button type="button" disabled={stale} onClick={() => session.setStressDisplay({ shown: !stress.display.shown })} className={`${NEUTRAL} disabled:cursor-not-allowed disabled:text-ink-disabled`}>
                {stress.display.shown && !stale ? "Modell zeigen" : "Heatmap zeigen"}
              </button>
              <button type="button" onClick={() => session.discardStressResult()} className={NEUTRAL}>
                Ergebnis verwerfen
              </button>
            </div>
          </Section>
        ) : null}
      </div>
    </aside>
  );
}
