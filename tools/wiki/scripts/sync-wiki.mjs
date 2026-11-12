// Fills tools/wiki/wiki-content/ with the GitHub Wiki pages (https://github.com/thnak/Tessera/wiki).
//   TESSERA_WIKI_DIR=<path>  copy a local checkout (use this to preview unpushed pages)
//   otherwise                shallow-clone or fast-forward Tessera.wiki.git into .wiki-cache/
// Design docs, specs and ADRs are NOT served here; they stay in the source repo (docs/, decisions/).
import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const out = join(root, "wiki-content");
const cache = join(root, ".wiki-cache");
const remote = "https://github.com/thnak/Tessera.wiki.git";

function git(args, cwd) {
  execFileSync("git", args, { cwd, stdio: "inherit" });
}

let source = process.env.TESSERA_WIKI_DIR ? resolve(process.env.TESSERA_WIKI_DIR) : null;
if (source === null) {
  try {
    if (existsSync(join(cache, ".git"))) git(["pull", "--ff-only", "--quiet"], cache);
    else git(["clone", "--depth", "1", "--quiet", remote, cache], root);
    source = cache;
  } catch {
    if (!existsSync(cache)) {
      console.error("sync-wiki: cannot reach the wiki repo and there is no cache; set TESSERA_WIKI_DIR");
      process.exit(1);
    }
    console.warn("sync-wiki: offline, using the cached wiki checkout");
    source = cache;
  }
}

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
for (const name of readdirSync(source)) {
  if (name === ".git") continue;
  cpSync(join(source, name), join(out, name), { recursive: true });
}
console.log(`sync-wiki: ${source} -> wiki-content/`);
