// SPDX-License-Identifier: MIT
//
// The skins slice's acceptance check (issue #171, `designer-studio-v1.md` sections 5.1 and 5.2).
//
//   node scripts/ci/preview_skins.mjs
//
// **Why this is a Node check and not a browser test.** The slice's acceptance is "geometry and retint
// tests", and both are properties of *data*: a manifest either validates or it does not, the viewer
// rectangle is arithmetic on the manifest, and "no skin carries a colour" is a statement about the
// files. None of that needs a browser, a GPU or a Wasm module, so none of it is tested through one.
// What a browser would add - that the frame paints, that a theme switch retints it - is the Pages
// lane's and a human's, and this script does not claim it.
//
// **Why the validator is imported from `dist-ssr/` and not from `designer/web/src`.** The same reason
// `preview_page_checks.mjs` renders the chrome from the built bundle (ADR-054 decision 6): the rules
// that ship are the rules that must be tested, and a source-only check cannot see a build that drops
// a module or mangles an export. `npm run build` is a prerequisite; this script says so when the
// artifact is missing rather than failing with a stack trace.
//
// **Why the manifests are read from `public/skins/` and not from `dist/`.** They are static files
// Vite copies verbatim, so the source directory is the same bytes the site serves - and reading it
// means a manifest is checked even on a lane that has not built the page. The one thing that *is*
// read from `dist/` is the validator, because that is the half a build can break.
//
// The checks, in the order the spec states them:
//
//  1. **Every bundled manifest parses and validates.** The seven skins section 5.2 names, each with
//     the fields the manifest requires. A missing directory is a failure, not a skip: the spec names
//     the set, so a skin that is not there is a skin the picker cannot offer.
//  2. **A manifest with a missing required field is rejected.** One case per required field, so a
//     validator that stopped checking `insets` fails on `insets` rather than on a generic "invalid".
//  3. **A manifest with an inset larger than its screen is rejected.** The spec's own example, plus
//     the two-insets-each-smaller case that a naive per-side check would let through.
//  4. **A negative size is rejected.** And a zero one, and a non-finite one: "negative size" is the
//     spec's shorthand for "not a rectangle".
//  5. **The derived viewer size equals `size` minus `bezel`, for each category.** The geometry
//     contract of section 5.1, asserted per category so a mode that forgot the bezel fails on its own
//     row.
//  6. **The retint rule: chrome colours come from the theme, not the manifest.** No bundled manifest
//     carries a colour field, and a manifest that does is rejected rather than ignored - an ignored
//     colour would ship as a frame that never retints.
//  7. **The three geometry modes of section 5.1.** `system` puts the title strip outside the canvas
//     and reports zero insets, `custom` draws no strip (the engine draws it) and no insets, `mobile`
//     overlays the bars and reports the skin's insets. This is the half of "geometry" that decides
//     what the page draws, so it is asserted rather than left to the renderer.
//  8. **The loader turns a missing manifest into a rejection.** Driven through the real
//     `loadBundledSkins` with a stub `fetch`, because "a bad skin is not offered" is a behaviour of
//     the loader and not only of the validator.
//
// Deterministic: no clock, no network, no filesystem order dependence (the skin list is the spec's
// order, and every assertion is over a fixed fixture). Exit codes: 0 all checks passed, 1 at least
// one failed, 2 setup error (the build is missing).

import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const WEB = resolve(REPO_ROOT, "designer", "web");
const DIST_SSR = resolve(WEB, "dist-ssr");
const SKINS = resolve(WEB, "public", "skins");

let failures = 0;

function fail(message) {
  process.stdout.write(`FAIL ${message}\n`);
  failures += 1;
}

function ok(message) {
  process.stdout.write(`ok   ${message}\n`);
}

/** Runs one named check; a throw is a failure with the thrown message, not a crashed script. */
function check(name, body) {
  try {
    body();
    ok(name);
  } catch (error) {
    fail(`${name}\n     ${error && error.message ? error.message : error}`);
  }
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

// --- the built validator, or a setup error that names the command ----------------------------------

for (const [path, what] of [
  [resolve(DIST_SSR, "skins.js"), "the built skin validator"],
  [resolve(DIST_SSR, "skin-geometry.js"), "the built skin geometry"],
  [resolve(DIST_SSR, "skin-load.js"), "the built skin loader"],
]) {
  try {
    readFileSync(path);
  } catch {
    process.stderr.write(
      `SETUP ERROR: ${what} is missing (${path}). Build the page first:\n` +
        "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
    );
    process.exit(2);
  }
}

const skins = await import(pathToFileURL(resolve(DIST_SSR, "skins.js")).href);
const geometry = await import(pathToFileURL(resolve(DIST_SSR, "skin-geometry.js")).href);
const loader = await import(pathToFileURL(resolve(DIST_SSR, "skin-load.js")).href);

const { SKIN_IDS, parseSkin } = skins;
const { geometryModeOf, viewerOf, safeAreasOf, barsOf, outerBoxOf, cutoutRectOf } = geometry;
const { loadBundledSkins, skinManifestUrl } = loader;

// --- the manifests on disk -------------------------------------------------------------------------

/** Every skin directory under `public/skins/`, sorted, so a stray directory is visible. */
function skinDirectories() {
  return readdirSync(SKINS)
    .filter((name) => statSync(resolve(SKINS, name)).isDirectory())
    .sort();
}

/** One manifest's parsed JSON, or a failure naming the file. */
function readManifest(id) {
  const path = resolve(SKINS, id, "skin.json");
  let text;
  try {
    text = readFileSync(path, "utf8");
  } catch {
    throw new Error(`skins/${id}/skin.json is missing`);
  }
  try {
    return JSON.parse(text);
  } catch (error) {
    throw new Error(`skins/${id}/skin.json is not valid JSON: ${error.message}`);
  }
}

/** A valid manifest to mutate, so a rejection case differs from a passing one in exactly one field. */
function validManifest() {
  return {
    schemaVersion: 1,
    name: "Fixture phone",
    category: "mobile",
    os: "android",
    size: { width: 412, height: 915 },
    bezel: { top: 12, right: 8, bottom: 12, left: 8, radius: 28 },
    cutout: { type: "punch-hole", x: 206, y: 14, radius: 5 },
    chrome: { statusBar: 24, navigationBar: 24, gestureBar: true },
    insets: { top: 24, right: 0, bottom: 24, left: 0 },
  };
}

/** `parseSkin` on a mutated fixture, as `{ok, reason}`. */
function parseFixture(mutate) {
  const document = validManifest();
  mutate(document);
  return parseSkin("fixture", document);
}

// --- 1. every bundled manifest parses and validates ------------------------------------------------

const bundled = new Map();

check("the bundled set is the seven skins section 5.2 names", () => {
  const onDisk = skinDirectories();
  assert(
    onDisk.length === SKIN_IDS.length,
    `public/skins/ holds ${onDisk.length} directories, the spec names ${SKIN_IDS.length}: ` +
      `${onDisk.join(", ")}`,
  );
  for (const id of SKIN_IDS) {
    assert(onDisk.includes(id), `public/skins/${id}/ is missing`);
  }
  for (const id of onDisk) {
    assert(SKIN_IDS.includes(id), `public/skins/${id}/ is not one of the spec's skins`);
  }
});

for (const id of SKIN_IDS) {
  check(`skins/${id}/skin.json parses and validates`, () => {
    const result = parseSkin(id, readManifest(id));
    assert(result.ok, result.ok ? "" : result.reason);
    bundled.set(id, result.skin);
  });
}

check("every bundled skin declares the manifest's required fields", () => {
  for (const id of SKIN_IDS) {
    const document = readManifest(id);
    // Section 5.2's table: `category`, `os`, `size` and `insets` are required; the rest are optional
    // and default. A bundled skin declares the optional ones anyway, because a bundled skin is the
    // example a reader copies - but the check is over the required set, which is what the validator
    // enforces.
    for (const field of ["category", "os", "size", "insets"]) {
      assert(
        document[field] !== undefined,
        `skins/${id}/skin.json does not declare \`${field}\``,
      );
    }
    assert(document.size.width > 0 && document.size.height > 0, `${id}: size is not positive`);
  }
});

check("the bundled set covers every category and every os the spec names", () => {
  const categories = new Set([...bundled.values()].map((skin) => skin.category));
  for (const category of ["desktop", "mobile", "tablet", "foldable"]) {
    assert(categories.has(category), `no bundled skin has category ${category}`);
  }
  const systems = new Set([...bundled.values()].map((skin) => skin.os));
  for (const os of ["windows", "macos", "linux", "android", "ios"]) {
    assert(systems.has(os), `no bundled skin has os ${os}`);
  }
});

check("the foldable skin is the one the fold posture control applies to", () => {
  const foldable = [...bundled.values()].find((skin) => skin.category === "foldable");
  assert(foldable !== undefined, "the bundled set has no foldable skin");
  // Section 6.1: the postures are `flat`, `book` and `tabletop`, and the foldable skin is what they
  // are previewed on. The skin itself carries no posture - the posture is a separate layout input -
  // so what this asserts is that the category exists for the control to name.
  assert(foldable.category === "foldable", "the foldable skin is not a foldable");
});

// --- 2. a missing required field is rejected -------------------------------------------------------

// Section 5.2's table: these four are the required fields. `bezel`, `cutout`, `chrome` and `art` are
// optional by the same table ("defaults to zero", "defaults by `os`", "the page draws a generated
// frame when absent"), and `schemaVersion`/`name` are not in the table at all - the version defaults
// to 1 and the name to the directory - so a manifest without them is valid and must stay valid.
const REQUIRED_FIELDS = ["category", "os", "size", "insets"];

for (const field of REQUIRED_FIELDS) {
  check(`a manifest without \`${field}\` is rejected`, () => {
    const result = parseFixture((document) => {
      delete document[field];
    });
    assert(!result.ok, `a manifest without \`${field}\` was accepted`);
    assert(
      result.code === "TESSERA_STUDIO_SKIN",
      `the rejection code is ${result.code}, not TESSERA_STUDIO_SKIN`,
    );
  });
}

check("the optional fields default rather than reject", () => {
  // The other half of the table: a manifest that declares only the required four is a valid skin,
  // and its defaults are the ones the table names.
  const result = parseSkin("fixture-minimal", {
    category: "desktop",
    os: "linux",
    size: { width: 1024, height: 768 },
    insets: { top: 0, right: 0, bottom: 0, left: 0 },
  });
  assert(result.ok, result.ok ? "" : result.reason);
  const skin = result.skin;
  assert(skin.schemaVersion === 1, `schemaVersion defaulted to ${skin.schemaVersion}`);
  assert(skin.name === "fixture-minimal", `name defaulted to ${skin.name}`);
  assert(skin.art === null, "art defaulted to something other than null");
  assert(
    skin.bezel.top === 0 && skin.bezel.right === 0 && skin.bezel.bottom === 0 && skin.bezel.left === 0,
    "bezel did not default to zero",
  );
  assert(skin.cutout.type === "none", `cutout defaulted to ${skin.cutout.type}`);
  // `chrome` defaults by `os`: a Linux window gets a title bar and no mobile bars.
  assert(skin.chrome.titleBar > 0, "a Linux skin defaulted to no title bar");
  assert(skin.chrome.statusBar === 0, "a Linux skin defaulted to a status bar");
  assert(
    skin.viewer.width === 1024 && skin.viewer.height === 768,
    "a bezel-less skin's viewer is not its size",
  );
});

check("a manifest with a missing nested field is rejected", () => {
  for (const [field, mutate] of [
    ["size.width", (document) => delete document.size.width],
    ["size.height", (document) => delete document.size.height],
    ["insets.top", (document) => delete document.insets.top],
    ["insets.left", (document) => delete document.insets.left],
    ["bezel.radius", (document) => delete document.bezel.radius],
    ["cutout.radius", (document) => delete document.cutout.radius],
  ]) {
    const result = parseFixture(mutate);
    assert(!result.ok, `a manifest without ${field} was accepted`);
  }
});

// --- 3. an inset larger than the screen is rejected ------------------------------------------------

check("an inset larger than the screen is rejected", () => {
  // The spec's own example: a 500 px inset on a 412 px screen.
  const result = parseFixture((document) => {
    document.insets = { top: 500, right: 0, bottom: 0, left: 0 };
  });
  assert(!result.ok, "a 500 px top inset on a 412x915 screen was accepted");
  assert(result.reason.includes("inset"), `the reason does not name the inset: ${result.reason}`);
});

check("two insets that are each smaller than the screen but together exceed it are rejected", () => {
  // The case a naive per-side check lets through: 300 + 300 on a 412 px screen leaves nothing.
  const result = parseFixture((document) => {
    document.insets = { top: 0, right: 300, bottom: 0, left: 300 };
  });
  assert(!result.ok, "300+300 horizontal insets on a 412 px screen were accepted");
});

check("an inset equal to the screen is rejected", () => {
  // `>=`, not `>`: an inset that consumes the whole screen leaves a zero-width viewer, which the
  // engine cannot lay out.
  const result = parseFixture((document) => {
    document.insets = { top: 0, right: 0, bottom: 0, left: 396 };
    document.insets.right = 396;
  });
  assert(!result.ok, "insets that consume the whole screen were accepted");
});

check("a bezel that leaves no screen is rejected", () => {
  const result = parseFixture((document) => {
    document.bezel = { top: 0, right: 206, bottom: 0, left: 206, radius: 0 };
  });
  assert(!result.ok, "a bezel wider than the device was accepted");
});

// --- 4. a negative size is rejected ----------------------------------------------------------------

check("a negative size is rejected", () => {
  for (const [field, value] of [
    ["width", -412],
    ["height", -915],
    ["width", 0],
    ["height", 0],
  ]) {
    const result = parseFixture((document) => {
      document.size[field] = value;
    });
    assert(!result.ok, `a size of ${field}=${value} was accepted`);
  }
});

check("a non-finite size is rejected", () => {
  // JSON cannot carry NaN or Infinity, but a manifest is data a person can hand-edit and a caller can
  // build in memory, so the validator refuses them rather than trusting the parser.
  for (const value of [Number.NaN, Number.POSITIVE_INFINITY]) {
    const result = parseFixture((document) => {
      document.size.width = value;
    });
    assert(!result.ok, `a size of width=${value} was accepted`);
  }
});

check("a negative inset is rejected", () => {
  const result = parseFixture((document) => {
    document.insets.top = -24;
  });
  assert(!result.ok, "a negative inset was accepted");
});

// --- 5. the derived viewer size is `size` minus `bezel`, per category ------------------------------

check("the derived viewer size is size minus bezel, for every bundled skin", () => {
  for (const [id, skin] of bundled) {
    const expected = {
      width: skin.size.width - skin.bezel.left - skin.bezel.right,
      height: skin.size.height - skin.bezel.top - skin.bezel.bottom,
    };
    assert(
      skin.viewer.width === expected.width && skin.viewer.height === expected.height,
      `${id}: viewer is ${skin.viewer.width}x${skin.viewer.height}, ` +
        `size - bezel is ${expected.width}x${expected.height}`,
    );
    const viewer = viewerOf(skin);
    assert(
      viewer.width === expected.width && viewer.height === expected.height,
      `${id}: viewerOf disagrees with the manifest's viewer`,
    );
  }
});

check("the derived viewer size is size minus bezel, for each category", () => {
  // One fixture per category, so a mode that forgot the bezel fails on its own row rather than being
  // masked by the desktop skins (whose bezel is zero).
  const fixtures = {
    desktop: {
      category: "desktop",
      os: "windows",
      size: { width: 1280, height: 800 },
      bezel: { top: 4, right: 6, bottom: 8, left: 10, radius: 8 },
      insets: { top: 0, right: 0, bottom: 0, left: 0 },
    },
    mobile: {
      category: "mobile",
      os: "android",
      size: { width: 412, height: 915 },
      bezel: { top: 12, right: 8, bottom: 12, left: 8, radius: 28 },
      insets: { top: 24, right: 0, bottom: 24, left: 0 },
    },
    tablet: {
      category: "tablet",
      os: "android",
      size: { width: 800, height: 1280 },
      bezel: { top: 16, right: 16, bottom: 16, left: 16, radius: 18 },
      insets: { top: 24, right: 0, bottom: 24, left: 0 },
    },
    foldable: {
      category: "foldable",
      os: "android",
      size: { width: 884, height: 1104 },
      bezel: { top: 14, right: 14, bottom: 14, left: 14, radius: 20 },
      insets: { top: 24, right: 0, bottom: 24, left: 0 },
    },
  };
  for (const [category, fixture] of Object.entries(fixtures)) {
    const result = parseSkin(`fixture-${category}`, {
      schemaVersion: 1,
      name: `Fixture ${category}`,
      ...fixture,
    });
    assert(result.ok, `${category}: ${result.ok ? "" : result.reason}`);
    const skin = result.skin;
    assert(
      skin.viewer.width === fixture.size.width - fixture.bezel.left - fixture.bezel.right,
      `${category}: viewer width is ${skin.viewer.width}`,
    );
    assert(
      skin.viewer.height === fixture.size.height - fixture.bezel.top - fixture.bezel.bottom,
      `${category}: viewer height is ${skin.viewer.height}`,
    );
  }
});

check("a skin with no bezel has a viewer equal to its size", () => {
  const result = parseSkin("fixture-bare", {
    schemaVersion: 1,
    name: "Bare",
    category: "desktop",
    os: "linux",
    size: { width: 1024, height: 768 },
    insets: { top: 0, right: 0, bottom: 0, left: 0 },
  });
  assert(result.ok, result.ok ? "" : result.reason);
  assert(
    result.skin.viewer.width === 1024 && result.skin.viewer.height === 768,
    `a bezel-less skin's viewer is ${result.skin.viewer.width}x${result.skin.viewer.height}`,
  );
});

// --- 6. the retint rule: no manifest carries a colour ---------------------------------------------

check("no bundled manifest carries a colour field", () => {
  // The rule is section 5.2's: "Chrome colours are not part of the skin ... A skin that hard-codes a
  // colour is a bug". The check is over the raw JSON, not the validated skin, because the validator
  // refuses a colour and a refusal would otherwise hide the fact that a manifest tried.
  const COLOUR_KEYS = [
    "color",
    "colour",
    "colors",
    "colours",
    "background",
    "foreground",
    "bg",
    "fg",
    "fill",
    "stroke",
    "tint",
    "scheme",
  ];
  const walk = (value, path, id) => {
    if (Array.isArray(value)) {
      value.forEach((entry, index) => walk(entry, `${path}[${index}]`, id));
      return;
    }
    if (typeof value !== "object" || value === null) {
      return;
    }
    for (const [key, entry] of Object.entries(value)) {
      const here = path === "" ? key : `${path}.${key}`;
      assert(
        !COLOUR_KEYS.includes(key.toLowerCase()),
        `skins/${id}/skin.json carries \`${here}\`; chrome colours come from the theme`,
      );
      walk(entry, here, id);
    }
  };
  for (const id of SKIN_IDS) {
    walk(readManifest(id), "", id);
  }
});

check("a manifest that carries a colour is rejected, not silently ignored", () => {
  for (const key of ["color", "background", "tint"]) {
    const result = parseFixture((document) => {
      document.chrome[key] = "#1c1f24";
    });
    assert(!result.ok, `a manifest with chrome.${key} was accepted`);
    assert(
      result.reason.includes("colour") || result.reason.includes("color"),
      `the reason does not name the colour rule: ${result.reason}`,
    );
  }
});

check("the bundled art declares no colour of its own", () => {
  // The SVG frame is the other half of the retint rule: a literal fill or stroke in it would be a
  // colour the manifest validator cannot see, and the frame would not follow a theme switch.
  for (const id of SKIN_IDS) {
    const skin = bundled.get(id);
    if (skin === undefined || skin.art === null) {
      continue;
    }
    const path = resolve(SKINS, id, skin.art);
    let text;
    try {
      text = readFileSync(path, "utf8");
    } catch {
      throw new Error(`skins/${id}/${skin.art} is declared but missing`);
    }
    // `currentColor` is the theme's, and `none` is the absence of a paint: both are allowed.
    const literals = text.match(/(?:fill|stroke)="(?!none|currentColor)[^"]*"/g) ?? [];
    assert(
      literals.length === 0,
      `skins/${id}/${skin.art} hard-codes a paint: ${literals.join(", ")}`,
    );
    assert(
      !/#[0-9a-fA-F]{3,8}\b/.test(text.replace(/<!--[\s\S]*?-->/g, "")),
      `skins/${id}/${skin.art} contains a hex colour outside its comments`,
    );
  }
});

// --- 7. the three geometry modes of section 5.1 ---------------------------------------------------

check("the system mode puts the title strip outside the canvas and reports no insets", () => {
  const desktop = bundled.get("generic-desktop-windows");
  assert(desktop !== undefined, "the Windows desktop skin is missing");
  const mode = geometryModeOf(desktop, "system");
  assert(mode === "system", `a desktop skin under titleBar=system is ${mode}`);
  const bars = barsOf(desktop, mode);
  assert(bars.title === desktop.chrome.titleBar, `the system mode draws no title strip`);
  assert(bars.status === 0 && bars.navigation === 0, "the system mode overlays no bars");
  const insets = safeAreasOf(desktop);
  assert(
    insets.top === 0 && insets.right === 0 && insets.bottom === 0 && insets.left === 0,
    "a desktop skin reports non-zero insets; its canvas is the client area",
  );
  // The strips are outside the canvas, so the outer box is taller than the viewer by exactly them.
  const outer = outerBoxOf(desktop, mode);
  assert(
    outer.height === desktop.viewer.height + bars.title + bars.menu,
    `the outer box is ${outer.height}, the viewer plus strips is ` +
      `${desktop.viewer.height + bars.title + bars.menu}`,
  );
});

check("the custom mode draws no simulated title bar and reports no insets", () => {
  const desktop = bundled.get("generic-desktop-windows");
  assert(desktop !== undefined, "the Windows desktop skin is missing");
  const mode = geometryModeOf(desktop, "custom");
  assert(mode === "custom", `a desktop skin under titleBar=custom is ${mode}`);
  const bars = barsOf(desktop, mode);
  assert(
    bars.title === 0 && bars.menu === 0,
    "the custom mode draws a simulated title bar; the engine draws the framework one",
  );
  const insets = safeAreasOf(desktop);
  assert(insets.top === 0, "the custom mode reports a top inset");
  const outer = outerBoxOf(desktop, mode);
  assert(
    outer.height === desktop.viewer.height,
    "the custom mode's outer box is taller than its viewer",
  );
});

check("the mobile mode overlays the bars and reports the skin's insets", () => {
  const phone = bundled.get("generic-phone-android");
  assert(phone !== undefined, "the Android phone skin is missing");
  const mode = geometryModeOf(phone, "system");
  assert(mode === "mobile", `a phone skin is ${mode}, not mobile`);
  const bars = barsOf(phone, mode);
  assert(bars.status === phone.chrome.statusBar, "the mobile mode draws no status bar");
  assert(bars.navigation === phone.chrome.navigationBar, "the mobile mode draws no navigation bar");
  assert(bars.title === 0, "the mobile mode draws a title strip");
  const insets = safeAreasOf(phone);
  assert(
    insets.top === phone.insets.top && insets.bottom === phone.insets.bottom,
    "the mobile mode does not report the skin's insets",
  );
  // The bars are overlaid, so they add nothing to the outer box.
  const outer = outerBoxOf(phone, mode);
  assert(
    outer.height === phone.size.height,
    `the mobile outer box is ${outer.height}, the device is ${phone.size.height}`,
  );
});

check("a mobile skin's insets contain its bars and its cutout", () => {
  // Section 5.2's rule, asserted on the bundled set rather than only on the validator's fixtures: a
  // phone whose status bar is taller than its top inset would draw the bar over content the engine
  // believes is clear.
  for (const [id, skin] of bundled) {
    if (skin.category === "desktop") {
      continue;
    }
    assert(
      skin.insets.top >= skin.chrome.statusBar,
      `${id}: insets.top ${skin.insets.top} is smaller than the ${skin.chrome.statusBar} px status bar`,
    );
    assert(
      skin.insets.bottom >= skin.chrome.navigationBar,
      `${id}: insets.bottom ${skin.insets.bottom} is smaller than the ` +
        `${skin.chrome.navigationBar} px navigation bar`,
    );
    const rect = cutoutRectOf(skin);
    if (skin.cutout.type === "none") {
      assert(rect === null, `${id}: a skin with no cutout reports a rectangle`);
      continue;
    }
    assert(rect !== null, `${id}: a skin with a cutout reports no rectangle`);
    // The rectangle is in viewer coordinates, so it is inside the viewer. A notch hangs from an
    // edge, so only the part inside the display is a rectangle - a top notch centred at y=0 starts
    // at y=0, not at -radius.
    assert(
      rect.x >= 0 && rect.y >= 0,
      `${id}: the cutout rectangle starts outside the viewer (${rect.x}, ${rect.y})`,
    );
    assert(
      rect.x + rect.width <= skin.viewer.width && rect.y + rect.height <= skin.viewer.height,
      `${id}: the cutout rectangle ends outside the viewer`,
    );
    // ... and it is inside the inset band that reserves room for it, which is the rule the validator
    // enforces on the manifest and this asserts on the derived rectangle.
    if (rect.y === 0) {
      assert(
        rect.height <= skin.insets.top,
        `${id}: the cutout reaches ${rect.height} px down but insets.top is ${skin.insets.top}`,
      );
    }
    if (rect.y + rect.height === skin.viewer.height) {
      assert(
        rect.height <= skin.insets.bottom,
        `${id}: the cutout reaches ${rect.height} px up but insets.bottom is ${skin.insets.bottom}`,
      );
    }
  }
});

check("the desktop skins report no cutout rectangle", () => {
  for (const [id, skin] of bundled) {
    if (skin.category !== "desktop") {
      continue;
    }
    assert(cutoutRectOf(skin) === null, `${id}: a desktop skin reports a cutout rectangle`);
  }
});

// --- 8. the loader turns a bad skin into a rejection ----------------------------------------------

await (async () => {
  const name = "the loader keeps the valid skins and reports the invalid ones";
  try {
    const responses = new Map();
    for (const id of SKIN_IDS) {
      responses.set(skinManifestUrl(id), JSON.stringify(readManifest(id)));
    }
    // One skin's manifest is replaced by a document with an inset larger than its screen, and one is
    // missing entirely: the two ways a bundled skin can be wrong.
    const broken = SKIN_IDS[3];
    const missing = SKIN_IDS[5];
    responses.set(
      skinManifestUrl(broken),
      JSON.stringify({ ...readManifest(broken), insets: { top: 5000, right: 0, bottom: 0, left: 0 } }),
    );
    responses.delete(skinManifestUrl(missing));

    const fetchImpl = async (url) => {
      if (!responses.has(url)) {
        return { ok: false, status: 404, text: async () => "" };
      }
      return { ok: true, status: 200, text: async () => responses.get(url) };
    };
    const loaded = await loadBundledSkins(fetchImpl);
    assert(
      loaded.skins.length === SKIN_IDS.length - 2,
      `the loader kept ${loaded.skins.length} skins, expected ${SKIN_IDS.length - 2}`,
    );
    assert(
      loaded.rejected.length === 2,
      `the loader reported ${loaded.rejected.length} rejections, expected 2`,
    );
    const rejectedIds = loaded.rejected.map((entry) => entry.id).sort();
    assert(
      rejectedIds.join(",") === [broken, missing].sort().join(","),
      `the loader rejected ${rejectedIds.join(", ")}, expected ${broken} and ${missing}`,
    );
    for (const entry of loaded.rejected) {
      assert(
        entry.code === "TESSERA_STUDIO_SKIN",
        `${entry.id}: the rejection code is ${entry.code}`,
      );
      assert(entry.reason.length > 0, `${entry.id}: the rejection carries no reason`);
    }
    ok(name);
  } catch (error) {
    fail(`${name}\n     ${error && error.message ? error.message : error}`);
  }
})();

// --- the canvas is never remounted by a skin ---------------------------------------------------------
//
// React matches children by position. If the canvas stack sat at a different path under a bare frame than
// under a skin, loading or picking a skin would unmount the `<canvas>` and mount a new one, and the
// session (which caches the element at creation) would keep painting into the detached node. No DOM runs
// here, so the check reads the element tree `SkinFrame` returns and compares the path to its children.

await (async () => {
  const name = "the canvas keeps one position in the frame with and without a skin";
  try {
  const { SkinFrame } = await import(pathToFileURL(resolve(DIST_SSR, "skin-frame.js")).href);
  const child = { marker: "canvas-stack" };
  const pathTo = (node, trail = []) => {
    if (node === child) return trail;
    if (node === null || typeof node !== "object" || !node.props) return null;
    const kids = Array.isArray(node.props.children) ? node.props.children : [node.props.children];
    for (let index = 0; index < kids.length; index += 1) {
      const found = pathTo(kids[index], [...trail, `${typeof node.type === "string" ? node.type : node.type.name}[${index}]`]);
      if (found !== null) return found;
    }
    return null;
  };
  const bare = pathTo(SkinFrame({ skin: null, mode: "mobile", children: child }));
  assert(bare !== null, "the bare frame does not render its children");
  for (const id of ["generic-phone-android", "generic-tablet", "generic-desktop-windows", "generic-foldable"]) {
    const result = parseSkin(id, readManifest(id));
    assert(result.ok, `${id}: ${result.ok ? "" : result.reason}`);
    const mode = geometryModeOf(result.skin, null);
    const skinned = pathTo(SkinFrame({ skin: result.skin, mode, children: child }));
    assert(skinned !== null, `${id}: the skinned frame does not render its children`);
    assert(
      JSON.stringify(skinned) === JSON.stringify(bare),
      `${id}: the canvas sits at ${skinned.join(" > ")} under the skin but ${bare.join(" > ")} bare, so React remounts it`,
    );
  }
    ok(name);
  } catch (error) {
    fail(`${name}
     ${error && error.message ? error.message : error}`);
  }
})();

// --- the summary -----------------------------------------------------------------------------------

process.stdout.write(
  failures === 0 ? "\nskin checks: all passed\n" : `\nskin checks: ${failures} failed\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
