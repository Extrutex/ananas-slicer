// Werkstoffe für die Kraftverteilung. Die Voreinstellungen sind Richtwerte für gedruckte FDM-Teile bei Belastung
// in der Druckebene, keine Datenblattwerte: Hersteller, Farbe, Düsen- und Bauraumtemperatur, Wandanzahl und
// Trocknung verschieben sie spürbar. Wer es genau braucht, trägt eigene Messwerte (Zugversuch) ein.

import type { StressMaterialJson } from "../engine/protocol.ts";

export interface StressMaterial {
  /** Elastizitätsmodul in der Ebene, MPa. */
  youngsModulus: number;
  /** Streckgrenze (Zugfestigkeit des gedruckten Teils) in der Ebene, MPa. */
  yieldStrength: number;
  poissonRatio: number;
  /** Anteil von Steifigkeit und Zugfestigkeit in z (Schichtverbund), 0.05..1. */
  zFactor: number;
}

export interface MaterialPreset extends StressMaterial {
  id: string;
  label: string;
}

export const MATERIAL_PRESETS: readonly MaterialPreset[] = [
  { id: "pla", label: "PLA", youngsModulus: 3000, yieldStrength: 45, poissonRatio: 0.35, zFactor: 0.6 },
  { id: "petg", label: "PETG", youngsModulus: 2000, yieldStrength: 40, poissonRatio: 0.38, zFactor: 0.65 },
  { id: "abs", label: "ABS", youngsModulus: 2000, yieldStrength: 33, poissonRatio: 0.35, zFactor: 0.55 },
  { id: "asa", label: "ASA", youngsModulus: 1900, yieldStrength: 35, poissonRatio: 0.35, zFactor: 0.6 },
  { id: "pa-cf", label: "PA-CF", youngsModulus: 6000, yieldStrength: 70, poissonRatio: 0.35, zFactor: 0.4 },
];

export const CUSTOM_MATERIAL_ID = "custom";

export const DEFAULT_PRESET = MATERIAL_PRESETS[0]!;

/** Der Schlüssel der Voreinstellung, deren Werte das Material genau hat, sonst "custom". */
export function presetIdOf(material: StressMaterial): string {
  const found = MATERIAL_PRESETS.find(
    (preset) =>
      preset.youngsModulus === material.youngsModulus &&
      preset.yieldStrength === material.yieldStrength &&
      preset.poissonRatio === material.poissonRatio &&
      preset.zFactor === material.zFactor,
  );
  return found?.id ?? CUSTOM_MATERIAL_ID;
}

export const Z_FACTOR_MIN = 0.05;

/** Die Gründe, warum ein Material nicht an die Engine gehen darf (leer = in Ordnung). */
export function materialProblems(material: StressMaterial): string[] {
  const problems: string[] = [];
  if (!Number.isFinite(material.youngsModulus) || material.youngsModulus <= 0) problems.push("Der E-Modul muss größer als 0 sein.");
  if (!Number.isFinite(material.yieldStrength) || material.yieldStrength <= 0) problems.push("Die Streckgrenze muss größer als 0 sein.");
  if (!Number.isFinite(material.poissonRatio) || material.poissonRatio <= -1 || material.poissonRatio >= 0.5) {
    problems.push("Die Querkontraktionszahl muss zwischen −1 und 0,5 liegen.");
  }
  if (!Number.isFinite(material.zFactor) || material.zFactor < Z_FACTOR_MIN || material.zFactor > 1) {
    problems.push(`Der z-Faktor muss zwischen ${Z_FACTOR_MIN} und 1 liegen.`);
  }
  return problems;
}

export function materialToJson(material: StressMaterial): StressMaterialJson {
  return {
    youngs_modulus: material.youngsModulus,
    yield_strength: material.yieldStrength,
    poisson_ratio: material.poissonRatio,
    z_factor: material.zFactor,
  };
}
