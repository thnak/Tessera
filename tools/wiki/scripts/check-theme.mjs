// Fails when the wiki theme uses CSS outside the Tessera stylesheet V1 priorities
// (decisions/ADR-016-stylesheet-engine.md, item 10), so the theme stays loadable by the native engine.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const file = join(dirname(fileURLToPath(import.meta.url)), "..", "src", "theme", "tessera-theme.css");
const css = readFileSync(file, "utf8").replace(/\/\*[\s\S]*?\*\//g, "");

const banned = [
  [/display\s*:\s*(inline-)?grid/, "grid layout (V1 allows only a small subset)"],
  [/grid-[a-z-]+\s*:/, "grid properties"],
  [/:has\(/, ":has()"],
  [/@container/, "@container"],
  [/@scope/, "@scope"],
  [/@property/, "@property"],
  [/\b(color|oklch|oklab|lab|lch|display-p3)\(/, "wide-gamut/modern color functions"],
];

const problems = banned.filter(([re]) => re.test(css)).map(([, why]) => why);
if (problems.length > 0) {
  console.error(`tessera-theme.css uses features outside Tessera stylesheet V1: ${problems.join(", ")}`);
  process.exit(1);
}
console.log("theme: ok (within Tessera stylesheet V1 priorities)");
