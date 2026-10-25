// The theme is the design system's own (docs/design/tailwind.tokens.js); tokens.css supplies the variables.
const ks = require("../docs/design/tailwind.tokens.js");

module.exports = {
  content: ["./index.html", "./src/**/*.{ts,tsx}"],
  theme: { extend: ks.extend },
};
