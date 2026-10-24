import { useEffect, useMemo, useRef, useState } from "react";
import type { CockpitSession, CockpitState, ViewMode } from "../state/session.ts";
import { fitsBed } from "./bed.ts";
import { CLASS_ORDER, FACE_COLORS, type FaceClass } from "./inspect.ts";
import { OVER_COLOR, QUANTITIES, RAMP, countOver, legendBins, type Quantity } from "./heatmap.ts";
import { previewScale } from "./preview.ts";
import { ViewerScene, type CameraPreset } from "./ViewerScene.ts";

const BUTTON =
  "h-target rounded border px-2 font-data text-ks-table transition-colors duration-fast";
const IDLE = `${BUTTON} border-edge bg-surface-panel text-ink-secondary hover:bg-surface-hover`;
const ACTIVE = `${BUTTON} border-edge-strong bg-carbon-700 text-ink-strong`;

const MODES: readonly { id: ViewMode; label: string; hint: string }[] = [
  { id: "solid", label: "Solid", hint: "Shaded model" },
  { id: "mesh", label: "Mesh", hint: "Triangles as a wireframe" },
  { id: "inspect", label: "Inspect", hint: "Overhangs, bed contact" },
];

const CAMERAS: readonly { id: CameraPreset | "fit" | "bed"; label: string }[] = [
  { id: "iso", label: "Iso" },
  { id: "top", label: "Top" },
  { id: "front", label: "Front" },
  { id: "right", label: "Right" },
  { id: "under", label: "Under" },
  { id: "fit", label: "Fit" },
  { id: "bed", label: "Bed" },
];

const CLASS_LABEL: Readonly<Record<FaceClass, string>> = {
  ok: "No support needed",
  caution: "Near the threshold",
  overhang: "Needs support",
  contact: "On the bed",
};

const pct = (value: number): string => `${(value * 100).toFixed(value > 0 && value < 0.1 ? 1 : 0)} %`;

function Segmented<T extends string>({ label, value, options, onChange }: {
  label: string;
  value: T;
  options: readonly { id: T; label: string; hint?: string }[];
  onChange: (id: T) => void;
}) {
  return (
    <div role="group" aria-label={label} className="flex gap-px">
      {options.map((option) => (
        <button
          key={option.id}
          type="button"
          aria-pressed={value === option.id}
          title={option.hint}
          onClick={() => onChange(option.id)}
          className={value === option.id ? ACTIVE : IDLE}
        >
          {option.label}
        </button>
      ))}
    </div>
  );
}

function SliderRow({ label, value, min, max, step, unit, onChange }: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  unit?: string;
  onChange: (value: number) => void;
}) {
  return (
    <label className="flex items-center gap-2 font-data text-ks-table text-ink-muted">
      <span className="w-[11ch] whitespace-nowrap">{label}</span>
      <input
        type="range"
        aria-label={label}
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(event) => onChange(Number(event.target.value))}
        className="h-1 w-40 accent-[var(--ks-alu-200)]"
      />
      <span className="w-[8ch] text-right text-ink-secondary">
        {value}
        {unit ?? ""}
      </span>
    </label>
  );
}

function InspectLegend({ shares }: { shares: Readonly<Record<FaceClass, number>> | null }) {
  if (shares === null) return null;
  return (
    <ul aria-label="Inspection legend" className="m-0 list-none border border-edge bg-surface-panel/90 p-2 font-data text-ks-table" data-testid="inspect-legend">
      {CLASS_ORDER.map((name) => (
        <li key={name} className="flex items-center gap-2 py-px text-ink-secondary">
          <span aria-hidden="true" className="inline-block h-2.5 w-2.5 rounded-[1px]" style={{ background: FACE_COLORS[name] }} />
          <span className="w-[22ch]">{CLASS_LABEL[name]}</span>
          <span className="ml-auto text-ink-strong">{pct(shares[name])}</span>
        </li>
      ))}
    </ul>
  );
}

function HeatmapLegend({ state, session }: { state: CockpitState; session: CockpitSession }) {
  const { preview, resolved } = state;
  const data = preview.data;
  const limits = resolved?.limits?.values ?? null;
  const scale = useMemo(() => (data ? previewScale(data.summary, preview.quantity, limits) : null), [data, preview.quantity, limits]);
  const over = useMemo(() => {
    if (!data || !scale) return 0;
    const values = preview.quantity === "speed" ? data.extrusion.speed : preview.quantity === "flow" ? data.extrusion.flow : data.extrusion.accel;
    return countOver(values, scale);
  }, [data, scale, preview.quantity]);
  if (!data || !scale) return null;
  const unit = QUANTITIES.find((q) => q.id === preview.quantity)!.unit;
  const top = scale.limit !== null ? Math.min(scale.max, scale.limit) : scale.max;
  const bins = legendBins(scale);
  const decimals = preview.quantity === "flow" ? 1 : 0;
  const unitLabel = (value: number) => value.toLocaleString("en", { maximumFractionDigits: decimals, minimumFractionDigits: decimals }).replace(/,/g, " ");
  return (
    <div className="w-[300px] border border-edge bg-surface-panel/90 p-2 font-data text-ks-table" data-testid="heatmap-legend">
      <Segmented
        label="Heatmap quantity"
        value={preview.quantity}
        options={QUANTITIES.map((q) => ({ id: q.id, label: q.label }))}
        onChange={(quantity: Quantity) => session.setPreview({ quantity })}
      />
      <div className="mt-2 flex h-3 gap-px" role="img" aria-label={`${preview.quantity} from ${unitLabel(scale.min)} to ${unitLabel(top)} ${unit}`}>
        {bins.map((bin, index) => (
          <span key={RAMP[index]} className="flex-1" style={{ background: bin.color }} title={`${unitLabel(bin.from)} – ${unitLabel(bin.to)} ${unit}`} />
        ))}
      </div>
      <div className="mt-1 flex justify-between text-ink-secondary">
        <span>{unitLabel(scale.min)}</span>
        <span>{unitLabel((scale.min + top) / 2)}</span>
        <span>
          {unitLabel(top)} {unit}
        </span>
      </div>
      {scale.limit !== null ? (
        <div className="mt-1.5 flex items-center gap-2 text-ink-muted" data-testid="over-limit">
          <span
            aria-hidden="true"
            className="inline-block h-2.5 w-4 border border-signal-limit"
            style={{ background: `repeating-linear-gradient(135deg, ${OVER_COLOR} 0 2px, transparent 2px 4px)` }}
          />
          <span>
            Over limit {">"} {unitLabel(scale.limit)} {unit}
          </span>
          <span className={over > 0 ? "ml-auto text-signal-limit" : "ml-auto"}>{over.toLocaleString("en").replace(/,/g, " ")} segments</span>
        </div>
      ) : null}
    </div>
  );
}

export function Viewer({ session, state }: { session: CockpitSession; state: CockpitState }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const sceneRef = useRef<ViewerScene | null>(null);
  const [shares, setShares] = useState<Readonly<Record<FaceClass, number>> | null>(null);
  const [webglFailed, setWebglFailed] = useState(false);
  const { view, preview, objects, selected, bed, resolved } = state;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    let scene: ViewerScene;
    try {
      scene = new ViewerScene(canvas, {
        onSelect: (index) => session.selectObject(index),
        onMove: (index, position) => session.moveObject(index, position),
        onMoveEnd: () => void session.commitObject(),
      });
    } catch {
      setWebglFailed(true);
      return;
    }
    sceneRef.current = scene;
    const observer = new ResizeObserver(() => scene.resize());
    observer.observe(canvas);
    return () => {
      observer.disconnect();
      scene.dispose();
      sceneRef.current = null;
    };
  }, [session]);

  const threshold = Number(resolved?.config.support_threshold_angle);
  const limits = resolved?.limits?.values ?? null;

  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    scene.update({
      bed,
      objects,
      selected,
      mode: view.mode,
      explode: view.explode,
      target: view.target,
      preview:
        preview.data !== null
          ? {
              data: preview.data,
              quantity: preview.quantity,
              scale: previewScale(preview.data.summary, preview.quantity, limits),
              layerMin: preview.layerMin,
              layerMax: preview.layerMax,
              showTravel: preview.showTravel,
              explodeLayers: preview.explodeLayers,
            }
          : null,
      overhangThreshold: Number.isFinite(threshold) && threshold > 0 ? threshold : 30,
    });
    setShares(view.mode === "inspect" ? scene.inspectShares(selected ?? 0) : null);
  }, [bed, objects, selected, view, preview, limits, threshold]);

  const object = selected !== null ? objects[selected] : objects[0];
  const layers = preview.data?.summary.layers ?? [];
  const parts = object?.parts.length ?? 0;
  const outside = objects.some((candidate) => !fitsBed(candidate.box, bed));

  if (webglFailed) {
    return (
      <div role="alert" className="flex flex-1 items-center justify-center bg-surface-viewport p-6 font-data text-ks-table text-signal-limit">
        The 3D view needs WebGL, which this window does not offer.
      </div>
    );
  }

  return (
    <div className="relative min-h-0 flex-1 bg-surface-viewport" data-testid="viewer">
      <canvas ref={canvasRef} className="absolute inset-0 h-full w-full outline-none" data-testid="viewer-canvas" />

      <div className="pointer-events-none absolute inset-x-3 top-3 flex items-start justify-between">
        <div className="pointer-events-auto flex flex-col gap-2">
          <Segmented
            label="Show"
            value={view.target}
            options={[
              { id: "model", label: "Model" },
              { id: "gcode", label: "G-code" },
            ]}
            onChange={(target) => preview.data !== null && session.setViewTarget(target)}
          />
          {view.target === "model" ? (
            <Segmented label="View" value={view.mode} options={MODES} onChange={(mode) => session.setViewMode(mode)} />
          ) : null}
        </div>
        <div className="pointer-events-auto flex gap-px" role="group" aria-label="Camera">
          {CAMERAS.map((camera) => (
            <button
              key={camera.id}
              type="button"
              className={IDLE}
              onClick={() => (camera.id === "fit" ? sceneRef.current?.fit() : camera.id === "bed" ? sceneRef.current?.fitBed() : sceneRef.current?.preset(camera.id))}
            >
              {camera.label}
            </button>
          ))}
        </div>
      </div>

      {view.target === "model" && object ? (
        <div className="pointer-events-none absolute left-3 top-[84px] font-data text-ks-table text-ink-muted" data-testid="mesh-stats">
          {object.stats.facets.toLocaleString("en").replace(/,/g, " ")} triangles {"·"} {Math.round(object.stats.volume).toLocaleString("en").replace(/,/g, " ")} mm{"³"}
          {" · "}
          {object.stats.open_edges > 0 ? (
            <span className="text-signal-limit">{object.stats.open_edges} open edges</span>
          ) : (
            "closed"
          )}
          {object.stats.repaired.facets_removed + object.stats.repaired.edges_fixed > 0 ? (
            <span className="text-signal-caution"> {"·"} repaired on load</span>
          ) : null}
          {outside ? <span className="text-signal-limit"> {"·"} outside the print area</span> : null}
        </div>
      ) : null}

      <div className="pointer-events-auto absolute bottom-3 left-3 flex flex-col gap-2">
        {view.target === "model" && view.mode === "inspect" ? <InspectLegend shares={shares} /> : null}
        {view.target === "gcode" ? <HeatmapLegend state={state} session={session} /> : null}
      </div>

      <div className="pointer-events-auto absolute bottom-3 right-3 flex flex-col gap-1 border border-edge bg-surface-panel/90 p-2">
        {view.target === "model" && parts > 1 ? (
          <SliderRow label="Explode" value={Math.round(view.explode * 100)} min={0} max={100} step={1} unit=" %" onChange={(value) => session.setExplode(value / 100)} />
        ) : null}
        {view.target === "gcode" && layers.length > 0 ? (
          <>
            <SliderRow label="Layer from" value={preview.layerMin + 1} min={1} max={layers.length} step={1} onChange={(value) => session.setPreview({ layerMin: Math.min(value - 1, preview.layerMax) })} />
            <SliderRow label="Layer to" value={preview.layerMax + 1} min={1} max={layers.length} step={1} onChange={(value) => session.setPreview({ layerMax: Math.max(value - 1, preview.layerMin) })} />
            <SliderRow label="Explode" value={preview.explodeLayers} min={0} max={20} step={0.5} unit=" mm" onChange={(value) => session.setPreview({ explodeLayers: value })} />
            <label className="flex items-center gap-2 font-data text-ks-table text-ink-muted">
              <input type="checkbox" checked={preview.showTravel} onChange={(event) => session.setPreview({ showTravel: event.target.checked })} />
              Travel moves
            </label>
            <p className="m-0 font-data text-ks-micro text-ink-muted">
              z {layers[preview.layerMin]?.z.toFixed(2)} {"–"} {layers[preview.layerMax]?.z.toFixed(2)} mm
            </p>
          </>
        ) : null}
      </div>
    </div>
  );
}
