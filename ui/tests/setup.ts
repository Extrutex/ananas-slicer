import "@testing-library/jest-dom/vitest";

// jsdom has no canvas: say so quietly instead of printing "not implemented" for every WebGL attempt. The viewer
// falls back to its message; what it draws is checked in a real browser (npm run e2e).
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
