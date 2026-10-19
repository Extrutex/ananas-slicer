/**
 * ANANAS SLICER Tailwind theme extension.
 *
 * Usage (tailwind.config.js):
 *   const ks = require("./docs/design/tailwind.tokens.js");
 *   module.exports = { content: [...], theme: { extend: ks.extend } };
 *
 * tokens.css must be loaded as well: the role colours below resolve to CSS
 * custom properties, so the Tailwind classes and the wx colour map stay driven
 * by one set of values. Role colours are `var()` references, which means
 * Tailwind's `/opacity` modifier does not work on them — that is intended;
 * translucent state colours are what made the Orca dark map drift, the signal
 * surfaces are solid tokens instead.
 *
 * docs/design/preview/index.html inlines a copy of `extend` because the
 * preview may only load scripts from cdnjs / cdn.tailwindcss.com. Change both.
 */

const primitives = {
  carbon: {
    950: "#07090b",
    900: "#0b0e11",
    850: "#0f1317",
    800: "#141a1f",
    750: "#1a2127",
    700: "#222a31",
  },
  steel: {
    600: "#2e3840",
    500: "#3d4953",
    400: "#5c6b77",
  },
  alu: {
    300: "#8b99a4",
    200: "#b4c0c9",
    100: "#d8e0e6",
    50: "#eef3f6",
  },
  ramp: {
    0: "#4b62a8",
    1: "#5a6ea6",
    2: "#6a7aa2",
    3: "#7b879e",
    4: "#8d9599",
    5: "#a0a393",
    6: "#b3b18a",
    7: "#c6bf7e",
    8: "#d9cd6e",
    9: "#ebdb5c",
    10: "#fbe84a",
  },
};

const extend = {
  colors: {
    ...primitives,
    // Signals: data graphs, hard limits, active G-code path. Nothing else.
    signal: {
      path: "var(--ks-signal-path)",
      limit: "var(--ks-signal-limit)",
      caution: "var(--ks-signal-caution)",
      nominal: "var(--ks-signal-nominal)",
      "path-surface": "var(--ks-signal-path-surface)",
      "limit-surface": "var(--ks-signal-limit-surface)",
      "caution-surface": "var(--ks-signal-caution-surface)",
    },
    surface: {
      chrome: "var(--ks-bg-chrome)",
      app: "var(--ks-bg-app)",
      panel: "var(--ks-bg-panel)",
      raised: "var(--ks-bg-raised)",
      field: "var(--ks-bg-field)",
      hover: "var(--ks-bg-hover)",
      viewport: "var(--ks-bg-viewport)",
    },
    ink: {
      strong: "var(--ks-ink-strong)",
      DEFAULT: "var(--ks-ink)",
      secondary: "var(--ks-ink-secondary)",
      muted: "var(--ks-ink-muted)",
      disabled: "var(--ks-ink-disabled)",
      "on-signal": "var(--ks-ink-on-signal)",
    },
    edge: {
      subtle: "var(--ks-edge-subtle)",
      DEFAULT: "var(--ks-edge)",
      strong: "var(--ks-edge-strong)",
      field: "var(--ks-edge-field)",
      focus: "var(--ks-focus)",
    },
  },
  fontFamily: {
    ui: ["Inter", "IBM Plex Sans", "Segoe UI", "system-ui", "sans-serif"],
    data: ["IBM Plex Mono", "JetBrains Mono", "ui-monospace", "SF Mono", "Consolas", "monospace"],
  },
  fontSize: {
    "ks-micro": ["11px", { lineHeight: "14px", letterSpacing: "0.08em" }],
    "ks-table": ["12px", { lineHeight: "16px" }],
    "ks-ui": ["13px", { lineHeight: "18px" }],
    "ks-section": ["15px", { lineHeight: "20px" }],
    "ks-title": ["18px", { lineHeight: "24px" }],
    "ks-metric": ["24px", { lineHeight: "28px" }],
  },
  spacing: {
    row: "28px",
    "row-header": "32px",
    target: "24px",
    sidebar: "460px",
  },
  borderRadius: {
    none: "0",
    DEFAULT: "2px",
    ks: "2px",
  },
  borderWidth: {
    state: "3px",
  },
  transitionDuration: {
    fast: "120ms",
    base: "220ms",
  },
};

const tokens = { primitives, extend };

if (typeof module !== "undefined" && module.exports) {
  module.exports = tokens;
}
