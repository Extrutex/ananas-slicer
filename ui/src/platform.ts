// What only the desktop shell can do: ask the user for a file. In a plain browser there is no path to a file the
// user picks, so the UI falls back to its path field.

export interface Platform {
  /** True when the shell offers native file dialogs. */
  native: boolean;
  /** Asks for a model file; null if the user cancelled. */
  pickModel(): Promise<string | null>;
  /** Asks where to save a G-code and copies it there; the path it was saved to, or null if the user cancelled. */
  saveGcode(from: string, suggestedName: string): Promise<string | null>;
}

const MODEL_EXTENSIONS = ["stl", "obj", "3mf", "amf", "step", "stp"];

export const browserPlatform: Platform = {
  native: false,
  pickModel: () => Promise.resolve(null),
  saveGcode: () => Promise.resolve(null),
};

const tauriPlatform: Platform = {
  native: true,
  async pickModel() {
    const { open } = await import("@tauri-apps/plugin-dialog");
    const chosen = await open({ multiple: false, directory: false, filters: [{ name: "3D models", extensions: MODEL_EXTENSIONS }] });
    return typeof chosen === "string" ? chosen : null;
  },
  async saveGcode(from, suggestedName) {
    const { save } = await import("@tauri-apps/plugin-dialog");
    const { invoke } = await import("@tauri-apps/api/core");
    const target = await save({ defaultPath: suggestedName, filters: [{ name: "G-code", extensions: ["gcode"] }] });
    if (target === null) return null;
    await invoke("export_gcode", { from, to: target });
    return target;
  },
};

export function createPlatform(): Platform {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window ? tauriPlatform : browserPlatform;
}
