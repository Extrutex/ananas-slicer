// The 3D scene: the bed, the model's objects with shadows, ambient occlusion and crease edges, the three views
// (solid, mesh, inspect), the exploded view and the G-code toolpaths with their heatmap. WebGL only; every
// decision that can be taken without a GPU lives in the pure modules next to this file.

import {
  ACESFilmicToneMapping,
  AmbientLight,
  Box3,
  BufferAttribute,
  BufferGeometry,
  Color,
  DirectionalLight,
  EdgesGeometry,
  ArrowHelper,
  DoubleSide,
  Group,
  HalfFloatType,
  HemisphereLight,
  LineBasicMaterial,
  LineSegments,
  Material,
  Matrix4,
  Mesh,
  MeshBasicMaterial,
  MeshStandardMaterial,
  Object3D,
  PCFShadowMap,
  PerspectiveCamera,
  PlaneGeometry,
  PMREMGenerator,
  Raycaster,
  Scene,
  ShadowMaterial,
  ShapeGeometry,
  Shape,
  Sphere,
  SphereGeometry,
  Vector2,
  Vector3,
  WebGLRenderer,
  WebGLRenderTarget,
  WireframeGeometry,
  Plane,
} from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import { RoomEnvironment } from "three/examples/jsm/environments/RoomEnvironment.js";
import { LineMaterial } from "three/examples/jsm/lines/LineMaterial.js";
import { LineSegments2 } from "three/examples/jsm/lines/LineSegments2.js";
import { LineSegmentsGeometry } from "three/examples/jsm/lines/LineSegmentsGeometry.js";
import { EffectComposer } from "three/examples/jsm/postprocessing/EffectComposer.js";
import { GTAOPass } from "three/examples/jsm/postprocessing/GTAOPass.js";
import { OutputPass } from "three/examples/jsm/postprocessing/OutputPass.js";
import { RenderPass } from "three/examples/jsm/postprocessing/RenderPass.js";
import { toCreasedNormals } from "three/examples/jsm/utils/BufferGeometryUtils.js";
import type { ViewMode } from "../state/session.ts";
import { fitsBed, type Bed } from "./bed.ts";
import { OVER_COLOR, RAMP, TRAVEL_COLOR, binOf, type Quantity, type Scale } from "./heatmap.ts";
import { CLASS_ORDER, FACE_COLORS, classifyFaces, type FaceClass, type InspectResult } from "./inspect.ts";
import { rowMajor, worldMatrix, type SceneObject } from "./objects.ts";
import type { PreviewData, SegmentSet } from "./preview.ts";
import type { StressMesh } from "../stress/mesh.ts";
import { partOffsets } from "../stress/regions.ts";

Object3D.DEFAULT_UP.set(0, 0, 1);

const COLORS = {
  background: "#0e1216",
  plate: "#12171c",
  gridMinor: "#1c232a",
  gridMajor: "#2e3840",
  volume: "#3d4953",
  model: "#aab6bf",
  edge: "#05070a",
  wire: "#8b99a4",
  selected: "#4db9ff",
  limit: "#ff6f5e",
  limitSurface: "#2a1513",
};

/** Angle above which two faces meet in a visible edge. */
const CREASE_DEG = 32;

export interface ViewerHandlers {
  onSelect(index: number | null): void;
  /** The selected object is dragged over the bed; `position` is the new centre of its box. */
  onMove(index: number, position: [number, number]): void;
  onMoveEnd(index: number): void;
  /** Im Wählmodus der Kraftverteilung: Klick auf Dreieck `triangle` von Teil `part` des Objekts `object`. */
  onPickFace?(object: number, part: number, triangle: number): void;
}

/** Eine Lager- oder Lastfläche, wie der Viewport sie zeigt. */
export interface StressRegionView {
  id: number;
  kind: "support" | "load";
  /** Dreiecke über alle Teile des Objekts (wie bei der Engine). */
  triangles: readonly number[];
  box: { min: readonly [number, number, number]; max: readonly [number, number, number] } | null;
  force: readonly [number, number, number];
  armed: boolean;
}

export interface StressMarker {
  at: readonly [number, number, number];
  color: string;
}

export interface StressView {
  object: number;
  /** Ein Klick wählt Flächen statt das Objekt zu wählen oder zu ziehen. */
  picking: boolean;
  regions: readonly StressRegionView[];
  /** Das Ergebnisnetz; wenn gesetzt, ersetzt es das Modell. */
  mesh: StressMesh | null;
  markers: readonly StressMarker[];
  markerSize: number;
}

export interface PreviewView {
  data: PreviewData;
  quantity: Quantity;
  scale: Scale;
  layerMin: number;
  layerMax: number;
  showTravel: boolean;
  explodeLayers: number;
}

export interface ViewerInput {
  bed: Bed;
  objects: readonly SceneObject[];
  selected: number | null;
  mode: ViewMode;
  /** 0..1 */
  explode: number;
  target: "model" | "gcode";
  preview: PreviewView | null;
  /** Support threshold of the profile, degrees, for the inspection view. */
  overhangThreshold: number;
  stress: StressView | null;
}

interface ObjectView {
  object: SceneObject;
  group: Group;
  parts: { mesh: Mesh; centre: Vector3; geometry: BufferGeometry }[];
  inspect: { key: string; result: InspectResult } | null;
  outline: LineSegments;
}

export type CameraPreset = "iso" | "top" | "front" | "right" | "under";

export class ViewerScene {
  readonly #canvas: HTMLCanvasElement;
  readonly #handlers: ViewerHandlers;
  readonly #renderer: WebGLRenderer;
  readonly #scene = new Scene();
  readonly #camera = new PerspectiveCamera(32, 1, 1, 5000);
  readonly #controls: OrbitControls;
  readonly #composer: EffectComposer | null;
  readonly #gtao: GTAOPass | null;
  readonly #bedGroup = new Group();
  readonly #modelGroup = new Group();
  readonly #previewGroup = new Group();
  readonly #stressGroup = new Group();
  readonly #light: DirectionalLight;
  readonly #raycaster = new Raycaster();
  #plate: Mesh | null = null;
  readonly #lineMaterials: LineMaterial[] = [];
  #views: ObjectView[] = [];
  #input: ViewerInput | null = null;
  #bed: Bed | null = null;
  #previewData: PreviewData | null = null;
  #drag: { index: number; start: Vector3; origin: [number, number] } | null = null;
  #disposed = false;
  /** Set when the first draw has happened with a model; tests wait for it. */
  drawn = 0;

  constructor(canvas: HTMLCanvasElement, handlers: ViewerHandlers) {
    this.#canvas = canvas;
    this.#handlers = handlers;
    this.#renderer = new WebGLRenderer({ canvas, antialias: true, powerPreference: "high-performance", preserveDrawingBuffer: true });
    this.#renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    this.#renderer.toneMapping = ACESFilmicToneMapping;
    this.#renderer.toneMappingExposure = 0.92;
    this.#renderer.shadowMap.enabled = true;
    this.#renderer.shadowMap.type = PCFShadowMap;
    this.#scene.background = new Color(COLORS.background);

    // Soft reflections: a studio environment lights the material, so curved parts read as curved.
    const pmrem = new PMREMGenerator(this.#renderer);
    this.#scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
    this.#scene.environmentIntensity = 0.42;
    pmrem.dispose();

    this.#scene.add(new HemisphereLight("#dfe8ee", "#1a2127", 0.35), new AmbientLight("#ffffff", 0.08));
    this.#light = new DirectionalLight("#fff4e8", 1.9);
    this.#light.castShadow = true;
    this.#light.shadow.mapSize.set(2048, 2048);
    this.#light.shadow.bias = -0.0004;
    this.#light.shadow.normalBias = 0.4;
    this.#light.shadow.radius = 5;
    this.#scene.add(this.#light, this.#light.target);
    this.#scene.add(this.#bedGroup, this.#modelGroup, this.#previewGroup, this.#stressGroup);

    this.#camera.up.set(0, 0, 1);
    this.#controls = new OrbitControls(this.#camera, canvas);
    this.#controls.enableDamping = false;
    this.#controls.maxPolarAngle = Math.PI; // the camera may go under the bed to look at the undersides
    this.#controls.addEventListener("change", () => this.#requestRender());

    // Ambient occlusion in screen space, with anti-aliased edges kept by a multisampled target.
    let composer: EffectComposer | null = null;
    let gtao: GTAOPass | null = null;
    try {
      const target = new WebGLRenderTarget(2, 2, { type: HalfFloatType, samples: 4 });
      composer = new EffectComposer(this.#renderer, target);
      composer.addPass(new RenderPass(this.#scene, this.#camera));
      gtao = new GTAOPass(this.#scene, this.#camera, 2, 2);
      gtao.output = GTAOPass.OUTPUT.Default;
      gtao.blendIntensity = 0.85;
      composer.addPass(gtao);
      composer.addPass(new OutputPass());
    } catch {
      composer = null;
      gtao = null;
    }
    this.#composer = composer;
    this.#gtao = gtao;

    canvas.addEventListener("pointerdown", this.#onPointerDown);
    canvas.addEventListener("pointermove", this.#onPointerMove);
    canvas.addEventListener("pointerup", this.#onPointerUp);
    this.resize();
  }

  // --- input ----------------------------------------------------------------------------------

  update(input: ViewerInput): void {
    const previous = this.#input;
    this.#input = input;
    if (previous === null || previous.bed !== input.bed) this.#buildBed(input.bed);
    if (previous === null || previous.objects !== input.objects || previous.mode !== input.mode || previous.overhangThreshold !== input.overhangThreshold) {
      this.#buildObjects(input);
    }
    this.#applySelection(input);
    this.#applyExplode(input);
    const stressMesh = input.stress?.mesh ?? null;
    this.#modelGroup.visible = (input.target === "model" || input.preview === null) && stressMesh === null;
    if (previous === null || previous.stress !== input.stress || previous.objects !== input.objects) this.#buildStress(input);
    this.#stressGroup.visible = input.target === "model" || input.preview === null;
    this.#canvas.style.cursor = input.stress?.picking ? "crosshair" : "";
    const previewKey = input.preview === null ? "" : [input.preview.quantity, input.preview.scale.min, input.preview.scale.max, input.preview.scale.limit, input.preview.layerMin, input.preview.layerMax, input.preview.showTravel, input.preview.explodeLayers].join("|");
    if (previous === null || previous.preview?.data !== input.preview?.data || this.#previewKey !== previewKey) {
      this.#previewKey = previewKey;
      this.#buildPreview(input);
    }
    this.#previewGroup.visible = input.target === "gcode" && input.preview !== null;
    const contentChanged =
      previous === null ||
      previous.objects[0]?.parts !== input.objects[0]?.parts ||
      previous.target !== input.target ||
      previous.preview?.data !== input.preview?.data;
    if (contentChanged) this.fit();
    this.#requestRender();
  }

  // --- bed ------------------------------------------------------------------------------------

  #buildBed(bed: Bed): void {
    const previousBed = this.#bed;
    this.#bed = bed;
    disposeChildren(this.#bedGroup);

    const shape = new Shape(bed.polygon.map(([x, y]) => new Vector2(x, y)));
    const plate = new Mesh(new ShapeGeometry(shape), new MeshStandardMaterial({ color: COLORS.plate, roughness: 0.92, metalness: 0 }));
    plate.position.z = -0.3;
    plate.receiveShadow = true;
    this.#plate = plate;
    this.#bedGroup.add(plate);

    // The shadow catcher: invisible except where the model's shadow falls.
    const width = bed.max[0] - bed.min[0];
    const depth = bed.max[1] - bed.min[1];
    const catcher = new Mesh(new PlaneGeometry(width, depth), new ShadowMaterial({ opacity: 0.45 }));
    catcher.position.set(bed.center[0], bed.center[1], 0.02);
    catcher.receiveShadow = true;
    this.#bedGroup.add(catcher);

    // Grid: 10 mm lines, every fifth one stronger, over the bed's bounding box.
    const minor: number[] = [];
    const major: number[] = [];
    const startX = Math.ceil(bed.min[0] / 10) * 10;
    for (let x = startX; x <= bed.max[0] + 1e-6; x += 10) (Math.round(x / 10) % 5 === 0 ? major : minor).push(x, bed.min[1], 0, x, bed.max[1], 0);
    const startY = Math.ceil(bed.min[1] / 10) * 10;
    for (let y = startY; y <= bed.max[1] + 1e-6; y += 10) (Math.round(y / 10) % 5 === 0 ? major : minor).push(bed.min[0], y, 0, bed.max[0], y, 0);
    this.#bedGroup.add(lines(minor, COLORS.gridMinor, 1), lines(major, COLORS.gridMajor, 1));

    // Outline of the build volume.
    const volume: number[] = [];
    const ring = bed.polygon;
    for (let i = 0; i < ring.length; i += 1) {
      const [x0, y0] = ring[i]!;
      const [x1, y1] = ring[(i + 1) % ring.length]!;
      volume.push(x0, y0, 0, x1, y1, 0, x0, y0, bed.height, x1, y1, bed.height, x0, y0, 0, x0, y0, bed.height);
    }
    this.#bedGroup.add(lines(volume, COLORS.volume, 0.55));

    // The light follows the bed's size so the shadow map always covers it.
    const span = Math.max(width, depth, bed.height);
    const centre = new Vector3(bed.center[0], bed.center[1], 0);
    this.#light.position.set(centre.x - span * 0.6, centre.y - span * 0.8, span * 1.3);
    this.#light.target.position.copy(centre);
    const shadow = this.#light.shadow.camera;
    const reach = span * 0.95;
    shadow.left = -reach;
    shadow.right = reach;
    shadow.top = reach;
    shadow.bottom = -reach;
    shadow.near = 1;
    shadow.far = span * 4;
    shadow.updateProjectionMatrix();
    this.#camera.far = span * 12;
    this.#camera.updateProjectionMatrix();
    this.#gtao?.updateGtaoMaterial({ radius: Math.max(4, span * 0.02), distanceExponent: 1.4, thickness: 1.2, scale: 1.1, samples: 16 });

    if (previousBed !== null && previousBed.polygon !== bed.polygon) this.#fitDirty = true;
  }

  #fitDirty = false;
  #previewKey = "";

  // --- objects --------------------------------------------------------------------------------

  #buildObjects(input: ViewerInput): void {
    for (const view of this.#views) this.#disposeView(view);
    this.#views = [];
    disposeChildren(this.#modelGroup);

    input.objects.forEach((object, index) => {
      const group = new Group();
      group.matrixAutoUpdate = false;
      group.userData.index = index;
      const parts: ObjectView["parts"] = [];
      for (const part of object.parts) {
        const indexed = new BufferGeometry();
        indexed.setAttribute("position", new BufferAttribute(part.positions, 3));
        indexed.setIndex(new BufferAttribute(part.indices, 1));
        // Smooth over curved surfaces, sharp across creases: the look of a CAD model, not a faceted mesh.
        const geometry = toCreasedNormals(indexed, (CREASE_DEG * Math.PI) / 180);
        indexed.dispose();
        geometry.computeBoundingBox();
        const centre = geometry.boundingBox!.getCenter(new Vector3());

        const material = new MeshStandardMaterial({ color: COLORS.model, roughness: 0.46, metalness: 0.12, envMapIntensity: 1 });
        const mesh = new Mesh(geometry, material);
        mesh.castShadow = true;
        mesh.receiveShadow = true;
        mesh.userData.index = index;
        mesh.userData.part = parts.length;
        const edges = new LineSegments(new EdgesGeometry(geometry, CREASE_DEG), new LineBasicMaterial({ color: COLORS.edge, transparent: true, opacity: 0.8 }));
        edges.name = "edges";
        mesh.add(edges);
        if (input.mode === "mesh") {
          material.transparent = true;
          material.opacity = 0.32;
          material.depthWrite = false;
          const wire = new LineSegments(new WireframeGeometry(geometry), new LineBasicMaterial({ color: COLORS.wire, transparent: true, opacity: 0.7 }));
          wire.name = "wire";
          mesh.add(wire);
          edges.visible = false;
        }
        group.add(mesh);
        parts.push({ mesh, centre, geometry });
      }
      const outline = new LineSegments(new BufferGeometry(), new LineBasicMaterial({ color: COLORS.selected, depthTest: false, transparent: true }));
      outline.renderOrder = 10;
      outline.frustumCulled = false;
      this.#modelGroup.add(group, outline);
      const view: ObjectView = { object, group, parts, inspect: null, outline };
      this.#views.push(view);
      group.matrix.copy(worldMatrix(object.transform, object.offset));
      group.matrixWorldNeedsUpdate = true;
      if (input.mode === "inspect") this.#colourByInspection(view, input.overhangThreshold);
    });
  }

  #disposeView(view: ObjectView): void {
    for (const part of view.parts) {
      part.geometry.dispose();
      (part.mesh.material as Material).dispose();
      part.mesh.traverse((child) => {
        if (child instanceof LineSegments) {
          child.geometry.dispose();
          (child.material as Material).dispose();
        }
      });
    }
    view.outline.geometry.dispose();
    (view.outline.material as Material).dispose();
  }

  /** Paints the faces of one object by what the printer will have to do for them. */
  #colourByInspection(view: ObjectView, threshold: number): void {
    const matrix = worldMatrix(view.object.transform, view.object.offset);
    const key = `${matrix.elements.map((v) => v.toFixed(5)).join(",")}|${threshold}`;
    if (view.inspect?.key !== key) {
      // One classification per part; the object's shares are the area-weighted mean.
      view.inspect = { key, result: classifyObject(view.object, rowMajor(matrix), threshold) };
    }
    let triangle = 0;
    for (const [index, part] of view.parts.entries()) {
      const mesh = view.object.parts[index]!;
      const count = mesh.indices.length / 3;
      const colours = new Float32Array(count * 9);
      const palette = CLASS_ORDER.map((name) => new Color(FACE_COLORS[name]));
      for (let t = 0; t < count; t += 1) {
        const colour = palette[view.inspect.result.classes[triangle + t]!]!;
        for (let k = 0; k < 3; k += 1) colours.set([colour.r, colour.g, colour.b], (t * 3 + k) * 3);
      }
      triangle += count;
      part.geometry.setAttribute("color", new BufferAttribute(colours, 3));
      // Flat, unlit colours: the legend's colours must be the ones on the model, whatever the light does.
      (part.mesh.material as Material).dispose();
      part.mesh.material = new MeshBasicMaterial({ vertexColors: true });
    }
  }

  /** The shares per class of the object the inspection view is showing, for the legend. */
  inspectShares(index: number): Readonly<Record<FaceClass, number>> | null {
    return this.#views[index]?.inspect?.result.share ?? null;
  }

  #applySelection(input: ViewerInput): void {
    this.#views.forEach((view, index) => {
      const selected = input.selected === index;
      const inside = fitsBed(view.object.box, input.bed);
      for (const part of view.parts) {
        const material = part.mesh.material;
        if (!(material instanceof MeshStandardMaterial)) continue; // the inspection view paints its own colours
        material.emissive.set(inside ? (selected ? "#0f2433" : "#000000") : COLORS.limitSurface);
        material.emissiveIntensity = inside ? (selected ? 0.7 : 0) : 1.6;
      }
      view.group.matrix.copy(worldMatrix(view.object.transform, view.object.offset));
      view.group.matrixWorldNeedsUpdate = true;
      const box = view.object.box;
      const outline = view.outline;
      outline.visible = selected || !inside;
      (outline.material as LineBasicMaterial).color.set(inside ? COLORS.selected : COLORS.limit);
      outline.geometry.dispose();
      outline.geometry = boxEdges(box.min, box.max);
    });
  }

  /** Pulls the parts of every object apart along the line from the object's centre to the part's centre. */
  #applyExplode(input: ViewerInput): void {
    for (const view of this.#views) {
      if (view.parts.length < 2) continue;
      const centre = view.parts.reduce((sum, part) => sum.add(part.centre), new Vector3()).divideScalar(view.parts.length);
      const radius = Math.max(...view.parts.map((part) => part.centre.distanceTo(centre)), 1e-6);
      const reach = new Box3().setFromObject(view.group).getSize(new Vector3()).length() * 0.5;
      for (const part of view.parts) {
        const direction = part.centre.clone().sub(centre);
        const length = direction.length();
        part.mesh.position.copy(length < 1e-6 ? new Vector3(0, 0, 0) : direction.multiplyScalar((reach * input.explode * 1.2) / Math.max(radius, length)));
      }
      view.group.updateMatrixWorld(true);
    }
  }

  // --- Kraftverteilung ------------------------------------------------------------------------

  #buildStress(input: ViewerInput): void {
    disposeChildren(this.#stressGroup);
    const stress = input.stress;
    if (stress === null) return;
    const object = input.objects[stress.object];

    if (stress.mesh !== null) {
      const geometry = new BufferGeometry();
      geometry.setAttribute("position", new BufferAttribute(stress.mesh.positions, 3));
      geometry.setAttribute("color", new BufferAttribute(stress.mesh.colors, 3));
      geometry.setIndex(new BufferAttribute(stress.mesh.indices, 1));
      // Flach und unlit: die Farben der Legende sind die Farben auf dem Teil.
      this.#stressGroup.add(new Mesh(geometry, new MeshBasicMaterial({ vertexColors: true, side: DoubleSide })));
      for (const marker of stress.markers) {
        const sphere = new Mesh(
          new SphereGeometry(stress.markerSize, 16, 12),
          new MeshBasicMaterial({ color: marker.color, depthTest: false, transparent: true, opacity: 0.95 }),
        );
        sphere.position.set(marker.at[0], marker.at[1], marker.at[2]);
        sphere.renderOrder = 20;
        this.#stressGroup.add(sphere);
      }
      return;
    }
    if (object === undefined) return;

    // Einrichtung: Lager blau, Lasten orange, die scharf gestellte Fläche kräftiger.
    const matrix = worldMatrix(object.transform, object.offset);
    const offsets = partOffsets(object.parts);
    for (const region of stress.regions) {
      const color = region.kind === "support" ? "#4db9ff" : "#ffb23e";
      const lookup = new Set(region.triangles);
      const points: number[] = [];
      const centroid = new Vector3();
      let count = 0;
      object.parts.forEach((part, p) => {
        const first = offsets[p]!;
        for (let t = 0; t < part.indices.length / 3; t += 1) {
          if (!lookup.has(first + t)) continue;
          for (let k = 0; k < 3; k += 1) {
            const v = new Vector3().fromArray(part.positions, part.indices[t * 3 + k]! * 3).applyMatrix4(matrix);
            points.push(v.x, v.y, v.z);
            centroid.add(v);
            count += 1;
          }
        }
      });
      if (points.length > 0) {
        const geometry = new BufferGeometry();
        geometry.setAttribute("position", new BufferAttribute(new Float32Array(points), 3));
        const overlay = new Mesh(
          geometry,
          new MeshBasicMaterial({ color, transparent: true, opacity: region.armed ? 0.85 : 0.6, side: DoubleSide, polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2 }),
        );
        this.#stressGroup.add(overlay);
        centroid.divideScalar(count);
      }
      if (region.box !== null) {
        const box = new LineSegments(boxEdges(region.box.min, region.box.max), new LineBasicMaterial({ color, depthTest: false, transparent: true }));
        box.renderOrder = 15;
        this.#stressGroup.add(box);
        if (points.length === 0) centroid.set((region.box.min[0] + region.box.max[0]) / 2, (region.box.min[1] + region.box.max[1]) / 2, (region.box.min[2] + region.box.max[2]) / 2);
      }
      const hasPlace = points.length > 0 || region.box !== null;
      const force = new Vector3(...region.force);
      if (region.kind === "load" && hasPlace && force.length() > 0) {
        const length = Math.max(12, Math.min(40, sizeOfBox(object.box) * 0.35));
        const arrow = new ArrowHelper(force.clone().normalize(), centroid, length, color, length * 0.28, length * 0.16);
        // Der Pfeil zeigt in Kraftrichtung und endet in der Fläche, damit man sieht, wo gedrückt oder gezogen wird.
        arrow.position.copy(centroid).addScaledVector(force.clone().normalize(), -length);
        arrow.traverse((node) => {
          const material = (node as Mesh).material as Material | undefined;
          if (material) material.depthTest = false;
          node.renderOrder = 16;
        });
        this.#stressGroup.add(arrow);
      }
    }
  }

  // --- toolpaths ------------------------------------------------------------------------------

  #buildPreview(input: ViewerInput): void {
    const preview = input.preview;
    if (preview === null) {
      this.#clearPreview();
      return;
    }
    // Rebuild the drawn range whenever anything the arrays depend on changes.
    this.#clearPreview();
    this.#previewData = preview.data;
    const from = Math.max(0, preview.layerMin);
    const to = Math.min(preview.data.summary.layers.length - 1, preview.layerMax);
    if (to < from) return;
    this.#previewGroup.add(this.#segmentsFor(preview.data.extrusion, from, to, (set, i) => colourOf(set, i, preview), preview.explodeLayers, 2.4));
    if (preview.showTravel) this.#previewGroup.add(this.#segmentsFor(preview.data.travel, from, to, () => new Color(TRAVEL_COLOR), preview.explodeLayers, 1));
  }

  #segmentsFor(set: SegmentSet, from: number, to: number, colour: (set: SegmentSet, index: number) => Color, gap: number, width: number): LineSegments2 {
    const start = set.layerStart[from]!;
    const end = set.layerStart[to + 1]!;
    const count = end - start;
    const positions = new Float32Array(count * 6);
    positions.set(set.positions.subarray(start * 6, end * 6));
    const colours = new Float32Array(count * 6);
    for (let i = 0; i < count; i += 1) {
      const c = colour(set, start + i);
      colours.set([c.r, c.g, c.b, c.r, c.g, c.b], i * 6);
      if (gap > 0) {
        const lift = (set.layer[start + i]! - from) * gap;
        positions[i * 6 + 2]! += lift;
        positions[i * 6 + 5]! += lift;
      }
    }
    const geometry = new LineSegmentsGeometry();
    geometry.setPositions(positions);
    geometry.setColors(colours);
    const material = new LineMaterial({ vertexColors: true, linewidth: width, worldUnits: false });
    material.resolution.set(this.#canvas.width, this.#canvas.height);
    this.#lineMaterials.push(material);
    const segments = new LineSegments2(geometry, material);
    segments.frustumCulled = false;
    return segments;
  }

  #clearPreview(): void {
    for (const child of [...this.#previewGroup.children]) {
      if (child instanceof LineSegments2) {
        child.geometry.dispose();
        (child.material as LineMaterial).dispose();
      }
      this.#previewGroup.remove(child);
    }
    this.#lineMaterials.length = 0;
    this.#previewData = null;
  }

  // --- camera ---------------------------------------------------------------------------------

  /** The box the camera frames: the toolpaths when they are shown, else the objects, else the bed. */
  #contentBox(): Box3 {
    const input = this.#input;
    const bed = this.#bed;
    const box = new Box3();
    if (input !== null && input.target === "gcode" && input.preview?.data.summary.bbox) {
      const b = input.preview.data.summary.bbox;
      const lift = input.preview.explodeLayers * Math.max(0, input.preview.layerMax - input.preview.layerMin);
      box.expandByPoint(new Vector3(b.min[0], b.min[1], b.min[2]));
      box.expandByPoint(new Vector3(b.max[0], b.max[1], b.max[2] + lift));
    } else if (input !== null) {
      for (const object of input.objects) {
        box.expandByPoint(new Vector3(...object.box.min));
        box.expandByPoint(new Vector3(...object.box.max));
      }
    }
    if (box.isEmpty() && bed !== null) {
      box.expandByPoint(new Vector3(bed.min[0], bed.min[1], 0));
      box.expandByPoint(new Vector3(bed.max[0], bed.max[1], 1));
    }
    return box;
  }

  /** Frames the content (objects, or the toolpaths in the G-code view). */
  fit(): void {
    this.preset("iso");
  }

  /** Frames the whole bed. */
  fitBed(): void {
    const bed = this.#bed;
    if (bed === null) return;
    this.preset("iso", new Box3(new Vector3(bed.min[0], bed.min[1], 0), new Vector3(bed.max[0], bed.max[1], 1)));
  }

  preset(name: CameraPreset, box?: Box3): void {
    if (this.#bed === null) return;
    const area = box ?? this.#contentBox();
    const centre = area.getCenter(new Vector3());
    // Distance at which the content's bounding sphere fills about two thirds of the view.
    const radius = Math.max(area.getBoundingSphere(new Sphere()).radius, 25);
    const fov = (this.#camera.fov * Math.PI) / 180;
    const distance = (radius / Math.sin(Math.min(fov, (fov * 2) / Math.max(this.#camera.aspect, 1)) / 2)) * 1.1;
    const direction = {
      iso: new Vector3(-0.55, -0.85, 0.75),
      top: new Vector3(0, -0.001, 1),
      front: new Vector3(0, -1, 0.18),
      right: new Vector3(1, 0, 0.18),
      under: new Vector3(-0.35, -0.6, -1),
    }[name].normalize();
    this.#controls.target.copy(centre);
    this.#camera.position.copy(centre).addScaledVector(direction, distance);
    this.#camera.lookAt(centre);
    this.#controls.update();
    this.#requestRender();
  }

  // --- interaction ----------------------------------------------------------------------------

  #pointer(event: PointerEvent): Vector2 {
    const rect = this.#canvas.getBoundingClientRect();
    return new Vector2(((event.clientX - rect.left) / rect.width) * 2 - 1, -((event.clientY - rect.top) / rect.height) * 2 + 1);
  }

  #pick(pointer: Vector2): { index: number; point: Vector3 } | null {
    this.#raycaster.setFromCamera(pointer, this.#camera);
    const meshes = this.#views.flatMap((view) => view.parts.map((part) => part.mesh));
    const hit = this.#raycaster.intersectObjects(meshes, false)[0];
    return hit ? { index: hit.object.userData.index as number, point: hit.point } : null;
  }

  /** Das Dreieck unter dem Zeiger: Objekt, Teil und Index im Teil. */
  #pickFace(pointer: Vector2): { object: number; part: number; triangle: number } | null {
    this.#raycaster.setFromCamera(pointer, this.#camera);
    const meshes = this.#views.flatMap((view) => view.parts.map((part) => part.mesh));
    const hit = this.#raycaster.intersectObjects(meshes, false)[0];
    if (!hit || hit.faceIndex === undefined || hit.faceIndex === null) return null;
    return { object: hit.object.userData.index as number, part: hit.object.userData.part as number, triangle: hit.faceIndex };
  }

  #onPointerDown = (event: PointerEvent): void => {
    if (event.button !== 0 || this.#input === null || this.#input.target !== "model") return;
    const pointer = this.#pointer(event);
    if (this.#input.stress?.picking) {
      // Wählmodus: der Klick wählt eine Fläche, er verschiebt und wählt kein Objekt.
      const face = this.#pickFace(pointer);
      if (face !== null) this.#handlers.onPickFace?.(face.object, face.part, face.triangle);
      return;
    }
    const hit = this.#pick(pointer);
    if (hit === null) {
      this.#handlers.onSelect(null);
      return;
    }
    this.#handlers.onSelect(hit.index);
    // A drag moves the object over the bed; orbiting pauses until the pointer is released.
    const object = this.#views[hit.index]!.object;
    this.#controls.enabled = false;
    this.#canvas.setPointerCapture(event.pointerId);
    this.#drag = { index: hit.index, start: this.#onBed(pointer) ?? hit.point, origin: [...object.transform.position] };
  };

  #onPointerMove = (event: PointerEvent): void => {
    if (this.#drag === null) return;
    const point = this.#onBed(this.#pointer(event));
    if (point === null) return;
    this.#handlers.onMove(this.#drag.index, [this.#drag.origin[0] + point.x - this.#drag.start.x, this.#drag.origin[1] + point.y - this.#drag.start.y]);
  };

  #onPointerUp = (event: PointerEvent): void => {
    if (this.#drag === null) return;
    const index = this.#drag.index;
    this.#drag = null;
    this.#controls.enabled = true;
    if (this.#canvas.hasPointerCapture(event.pointerId)) this.#canvas.releasePointerCapture(event.pointerId);
    this.#handlers.onMoveEnd(index);
  };

  /** Where the pointer's ray meets the plane of the bed. */
  #onBed(pointer: Vector2): Vector3 | null {
    this.#raycaster.setFromCamera(pointer, this.#camera);
    return this.#raycaster.ray.intersectPlane(new Plane(new Vector3(0, 0, 1), 0), new Vector3());
  }

  // --- drawing --------------------------------------------------------------------------------

  resize(): void {
    const width = Math.max(1, this.#canvas.clientWidth);
    const height = Math.max(1, this.#canvas.clientHeight);
    this.#renderer.setSize(width, height, false);
    this.#camera.aspect = width / height;
    this.#camera.updateProjectionMatrix();
    this.#composer?.setSize(width, height);
    this.#gtao?.setSize(width, height);
    for (const material of this.#lineMaterials) material.resolution.set(this.#canvas.width, this.#canvas.height);
    this.#requestRender();
  }

  #requestRender(): void {
    if (this.#disposed || this.#renderPending) return;
    this.#renderPending = true;
    requestAnimationFrame(() => {
      this.#renderPending = false;
      if (this.#disposed) return;
      if (this.#fitDirty) {
        this.#fitDirty = false;
        this.fit();
      }
      this.#render();
    });
  }

  #renderPending = false;

  #render(): void {
    // From below the bed lets the model show through, so the undersides can be inspected.
    if (this.#plate !== null) {
      const material = this.#plate.material as MeshStandardMaterial;
      const below = this.#camera.position.z < 0;
      if (material.transparent !== below) {
        material.transparent = below;
        material.needsUpdate = true;
      }
      material.opacity = below ? 0.18 : 1;
      material.depthWrite = !below;
    }
    if (this.#composer !== null) this.#composer.render();
    else this.#renderer.render(this.#scene, this.#camera);
    this.drawn += 1;
    // For tests and diagnostics: how often the scene has been drawn and where the camera is.
    this.#canvas.dataset.drawn = String(this.drawn);
    this.#canvas.dataset.camera = [...this.#camera.position.toArray(), ...this.#controls.target.toArray()].map((v) => v.toFixed(1)).join(",");
  }

  /** Number of triangles and segments in view, for the status line. */
  stats(): { triangles: number; segments: number } {
    const triangles = this.#views.reduce((sum, view) => sum + view.object.parts.reduce((s, part) => s + part.triangles, 0), 0);
    return { triangles, segments: this.#previewData?.extrusion.count ?? 0 };
  }

  dispose(): void {
    this.#disposed = true;
    this.#canvas.removeEventListener("pointerdown", this.#onPointerDown);
    this.#canvas.removeEventListener("pointermove", this.#onPointerMove);
    this.#canvas.removeEventListener("pointerup", this.#onPointerUp);
    this.#controls.dispose();
    for (const view of this.#views) this.#disposeView(view);
    this.#clearPreview();
    disposeChildren(this.#bedGroup);
    this.#composer?.dispose();
    this.#renderer.dispose();
  }
}

function classifyObject(object: SceneObject, matrix: number[], threshold: number): InspectResult {
  const classes: number[] = [];
  const area: Record<FaceClass, number> = { ok: 0, caution: 0, overhang: 0, contact: 0 };
  let total = 0;
  for (const part of object.parts) {
    const result = classifyFaces(part.positions, part.indices, matrix, threshold);
    classes.push(...result.classes);
    // Re-weight by the part's own total so shares add across parts by area, not by part count.
    const partArea = meshArea(part.positions, part.indices, matrix);
    for (const name of CLASS_ORDER) area[name] += result.share[name] * partArea;
    total += partArea;
  }
  const share = { ok: 0, caution: 0, overhang: 0, contact: 0 } as Record<FaceClass, number>;
  for (const name of CLASS_ORDER) share[name] = total > 0 ? area[name] / total : 0;
  return { classes: Uint8Array.from(classes), share };
}

function meshArea(positions: Float32Array, indices: Uint32Array, m: readonly number[]): number {
  let total = 0;
  const a = new Vector3(), b = new Vector3(), c = new Vector3();
  const matrix = new Matrix4().set(...(m as [number, number, number, number, number, number, number, number, number, number, number, number, number, number, number, number]));
  for (let t = 0; t < indices.length; t += 3) {
    a.fromArray(positions, indices[t]! * 3).applyMatrix4(matrix);
    b.fromArray(positions, indices[t + 1]! * 3).applyMatrix4(matrix);
    c.fromArray(positions, indices[t + 2]! * 3).applyMatrix4(matrix);
    total += b.sub(a).cross(c.sub(a)).length() / 2;
  }
  return total;
}

/** Colour of one extrusion segment under the current heatmap: the ramp bin of its value, or the limit colour above the limit. */
function colourOf(set: SegmentSet, index: number, preview: PreviewView): Color {
  const value = preview.quantity === "speed" ? set.speed[index]! : preview.quantity === "flow" ? set.flow[index]! : set.accel[index]!;
  const bin = binOf(value, preview.scale);
  return COLOUR_CACHE.get(bin === -1 ? OVER_COLOR : RAMP[bin]!);
}

const COLOUR_CACHE = {
  cache: new Map<string, Color>(),
  get(hex: string): Color {
    let colour = this.cache.get(hex);
    if (colour === undefined) {
      colour = new Color(hex);
      this.cache.set(hex, colour);
    }
    return colour;
  },
};

function lines(points: number[], colour: string, opacity: number): LineSegments {
  const geometry = new BufferGeometry();
  geometry.setAttribute("position", new BufferAttribute(new Float32Array(points), 3));
  return new LineSegments(geometry, new LineBasicMaterial({ color: colour, transparent: opacity < 1, opacity }));
}

function boxEdges(min: readonly number[], max: readonly number[]): BufferGeometry {
  const [x0, y0, z0] = min as [number, number, number];
  const [x1, y1, z1] = max as [number, number, number];
  const corners = [
    [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
    [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1],
  ] as const;
  const edges = [0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7];
  const points = edges.flatMap((corner) => corners[corner]!);
  const geometry = new BufferGeometry();
  geometry.setAttribute("position", new BufferAttribute(new Float32Array(points), 3));
  return geometry;
}

function sizeOfBox(box: { min: readonly number[]; max: readonly number[] }): number {
  return Math.max(box.max[0]! - box.min[0]!, box.max[1]! - box.min[1]!, box.max[2]! - box.min[2]!);
}

function disposeChildren(group: Group): void {
  for (const child of [...group.children]) {
    child.traverse((node) => {
      const mesh = node as Mesh;
      mesh.geometry?.dispose();
      const material = mesh.material as Material | Material[] | undefined;
      if (Array.isArray(material)) material.forEach((m) => m.dispose());
      else material?.dispose();
    });
    group.remove(child);
  }
}
