// The ananas-engine JSON-RPC protocol (docs/HLSD/engine.md), as the front end sees it.

export type Scope = "print" | "filament" | "printer";

export interface EngineInfo {
  name: string;
  version: string;
  protocol: number;
  methods: string[];
}

export interface SchemaOption {
  key: string;
  scope: Scope;
  type: string;
  vector: boolean;
  label: string;
  category: string;
  tooltip: string;
  unit: string;
  mode: string;
  readonly: boolean;
  nullable: boolean;
  min?: number;
  max?: number;
  enum_values?: string[];
  enum_labels?: string[];
  default?: string;
}

export interface PrinterPreset {
  name: string;
  vendor: string;
  model: string;
  variant: string;
  default_process: string;
}

export interface CompatiblePresets {
  printer: string;
  processes: string[];
  filaments: string[];
}

export interface KlipperLimitValues {
  max_velocity: number;
  max_accel: number;
  minimum_cruise_ratio: number;
  square_corner_velocity: number;
  max_z_velocity: number;
  max_z_accel: number;
  max_extrude_only_velocity: number;
  max_extrude_only_accel: number;
  instantaneous_corner_velocity: number;
  pressure_advance: number;
  pressure_advance_smooth_time: number;
}

/** The limits a config is checked against; `source` is "profile", "printer.cfg" or "moonraker". */
export interface EffectiveLimits {
  source: string;
  values: KlipperLimitValues;
}

export interface Violation {
  key: string;
  label: string;
  unit: string;
  kind: "machine" | "profile" | "firmware";
  state: "caution" | "exceeded";
  value: number;
  limit: number;
  limit_key: string;
  source: string;
}

/** Which presets and which imported Klipper limits a request works on. */
export interface Selection {
  printer: string;
  process?: string;
  filament?: string;
  overrides?: Record<string, string>;
  limits?: ImportedLimits;
}

/** Klipper limits imported from printer.cfg or Moonraker, sent back with every request. */
export type ImportedLimits = KlipperLimitValues & { source: "printer_cfg" | "moonraker" };

export interface ResolveResult {
  printer: string;
  process: string;
  filament: string;
  /** Every setting as its serialized string; a per-extruder value is comma separated. */
  config: Record<string, string>;
  limits: EffectiveLimits | null;
  violations: Violation[];
}

export interface LimitsImportResult {
  source: "printer_cfg" | "moonraker";
  limits: KlipperLimitValues;
  warnings: string[];
}

export interface SliceRequest extends Selection {
  model: string;
  output: string;
  /** One placement per object; without it the model keeps its layout and is centred. */
  transforms?: ObjectTransform[];
}

export interface SliceResult {
  gcode: string;
  time_s: number;
  layers: number;
  filament_mm: number;
  filament_g: number;
  limits: EffectiveLimits | null;
  violations: Violation[];
}

export interface SliceProgress {
  percent: number;
  message: string;
}

/** JSON-RPC error codes of the engine. */
export const ERROR_INVALID_PARAMS = -32602;
export const ERROR_NOT_FOUND = -32001;

export class EngineRpcError extends Error {
  readonly code: number;

  constructor(code: number, message: string) {
    super(message);
    this.name = "EngineRpcError";
    this.code = code;
  }
}

/** An object's placement on the bed (docs/HLSD/engine.md): position is the centre of its bounding box. */
export interface ObjectTransform {
  position?: [number, number];
  rotation?: [number, number, number];
  scale?: [number, number, number];
}

export interface BoxJson {
  min: [number, number, number];
  max: [number, number, number];
}

export interface MeshStats {
  facets: number;
  volume: number;
  open_edges: number;
  shells: number;
  repaired: {
    degenerate_facets: number;
    edges_fixed: number;
    facets_removed: number;
    facets_reversed: number;
    backwards_edges: number;
  };
}

export interface PartJson {
  name: string;
  vertices: string;
  indices: string;
  triangles: number;
  bbox: BoxJson;
}

export interface ObjectJson {
  name: string;
  /** Where the file puts the object; null when it has no instance. */
  placement: { position: [number, number]; rotation: [number, number, number]; scale: [number, number, number]; bbox: BoxJson } | null;
  parts: PartJson[];
  bbox: BoxJson;
  stats: MeshStats;
}

export interface ModelLoadResult {
  path: string;
  objects: ObjectJson[];
}

export interface BoundsResult {
  objects: { bbox: BoxJson; offset: [number, number, number] }[];
}

export interface Range {
  min: number;
  max: number;
}

export interface PreviewSummary {
  gcode: string;
  time_s: number;
  layers: { id: number; z: number }[];
  extrusion_segments: number;
  travel_segments: number;
  bbox: BoxJson | null;
  speed: Range;
  flow: Range;
  accel: Range;
  roles: { id: number; name: string; segments: number }[];
}

export interface PreviewChunkJson {
  count: number;
  positions?: string;
  speed?: string;
  flow?: string;
  accel?: string;
  role?: string;
  kind?: string;
  layer?: string;
}
