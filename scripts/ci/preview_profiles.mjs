// SPDX-License-Identifier: MIT
//
// The device-profile slice's acceptance check (issue #447, `studio-device-profiles-v1.md` sections 2 and 3).
//
//   node scripts/ci/preview_profiles.mjs
//
// Like `preview_skins.mjs` this is a Node check, because the acceptance is about data and rules: a manifest
// validates or it does not, a device switch restarts or it does not, and a link decodes to the state it
// always did. The validator, the loader and the switch rule are imported from `dist-ssr/` (the code the page
// ships); the share codec is a pure ES module and is imported from source, exactly as
// `preview_share_roundtrip.mjs` does. `npm run build` is a prerequisite; a missing build is exit code 2.
//
// The checks:
//
//  1. Every bundled manifest validates, the set is the spec's seven, and the codec's `PROFILE_GEOMETRY`
//     table agrees with each manifest (size and DPR) -- the table is a copy the codec needs, so a drift
//     fails here rather than as a link that will not decode.
//  2. Each profile's skin exists, and its size and `os` agree with the skin's.
//  3. A manifest with a bad field is rejected with the named error `TESSERA_STUDIO_PROFILE` and a reason
//     that names the field: one case per field.
//  4. The loader turns a missing and a broken manifest into rejections and keeps the rest.
//  5. Switching device (section 3): the default restarts; keep-state applies in place only inside one `os`;
//     a change of `os` always restarts, whatever the setting says.
//  6. The share link: a pre-existing `#p5=` fragment decodes unchanged (no `n`), a profile name round-trips
//     and widens only the size and DPR it owns, and an unknown name or a stray size is still refused.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const WEB = resolve(REPO_ROOT, "designer", "web");
const DIST_SSR = resolve(WEB, "dist-ssr");
const PROFILES = resolve(WEB, "public", "profiles");
const SKINS = resolve(WEB, "public", "skins");

let failures = 0;
const fail = (message) => {
  process.stdout.write(`FAIL ${message}\n`);
  failures += 1;
};
const ok = (message) => process.stdout.write(`ok   ${message}\n`);
const assert = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};

async function check(name, body) {
  try {
    await body();
    ok(name);
  } catch (error) {
    fail(`${name}\n     ${error && error.message ? error.message : error}`);
  }
}

for (const file of ["device-profile.js", "device-profile-load.js", "profiles-switch.js"]) {
  try {
    readFileSync(resolve(DIST_SSR, file));
  } catch {
    process.stderr.write(
      `SETUP ERROR: ${file} is missing from ${DIST_SSR}. Build the page first:\n` +
        "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
    );
    process.exit(2);
  }
}

const load = (file) => import(pathToFileURL(resolve(DIST_SSR, file)).href);
const { parseProfile, customProfile, mayKeepState } = await load("device-profile.js");
const { loadBundledProfiles, BUNDLED_PROFILE_IDS } = await load("device-profile-load.js");
const { deviceSwitch } = await load("profiles-switch.js");
const state = await import(pathToFileURL(resolve(WEB, "src", "lib", "preview-state.js")).href);
const share = await import(pathToFileURL(resolve(WEB, "src", "lib", "share.js")).href);

const SPEC_SET = [
  "android-phone",
  "ios-phone",
  "android-tablet",
  "foldable",
  "desktop-windows",
  "desktop-macos",
  "desktop-linux",
];

const manifest = (id) => JSON.parse(readFileSync(resolve(PROFILES, id, "profile.json"), "utf8"));
const skinManifest = (id) => JSON.parse(readFileSync(resolve(SKINS, id, "skin.json"), "utf8"));

await check("the bundled set is the spec's seven, on disk and in the vocabulary", () => {
  const onDisk = readdirSync(PROFILES).filter((name) => statSync(resolve(PROFILES, name)).isDirectory()).sort();
  assert(onDisk.join(",") === [...SPEC_SET].sort().join(","), `profiles/ holds ${onDisk.join(", ")}`);
  assert(BUNDLED_PROFILE_IDS.join(",") === SPEC_SET.join(","), `the loader offers ${BUNDLED_PROFILE_IDS.join(", ")}`);
});

await check("every bundled manifest validates and the codec's geometry table agrees with it", () => {
  for (const id of SPEC_SET) {
    const result = parseProfile(id, manifest(id));
    assert(result.ok, `${id}: ${result.reason}`);
    const row = state.PROFILE_GEOMETRY[id];
    assert(row !== undefined, `${id} is missing from PROFILE_GEOMETRY`);
    const { size, dpr } = result.profile;
    assert(
      row[0] === size.width && row[1] === size.height && row[2] === dpr,
      `${id}: the table says ${row.join("x")} but the manifest is ${size.width}x${size.height}@${dpr}`,
    );
  }
});

await check("each profile's skin exists and agrees with it on size and os", () => {
  for (const id of SPEC_SET) {
    const profile = parseProfile(id, manifest(id)).profile;
    assert(profile.skin !== null, `${id} references no skin`);
    const skin = skinManifest(profile.skin);
    assert(skin.os === profile.os, `${id}: os ${profile.os} but its skin says ${skin.os}`);
    assert(
      skin.size.width === profile.size.width && skin.size.height === profile.size.height,
      `${id}: size ${profile.size.width}x${profile.size.height} but its skin is ${skin.size.width}x${skin.size.height}`,
    );
  }
});

await check("the strip is generated from `controls`: a desktop offers none, a phone its six", () => {
  assert(parseProfile("d", manifest("desktop-windows")).profile.controls.length === 0, "a desktop lists controls");
  const phone = parseProfile("p", manifest("android-phone")).profile;
  assert(
    phone.controls.join(",") === "back,home,recents,volumeUp,volumeDown,power",
    `the phone lists ${phone.controls.join(", ")}`,
  );
  const ios = parseProfile("i", manifest("ios-phone")).profile;
  assert(!ios.controls.includes("back") && !ios.controls.includes("recents"), "an iPhone lists Back or Recents");
});

const good = () => structuredClone(manifest("android-phone"));
const bad = [
  ["schemaVersion", (m) => { m.schemaVersion = 2; }],
  ["name", (m) => { m.name = ""; }],
  ["category", (m) => { m.category = "watch"; }],
  ["os", (m) => { m.os = "symbian"; }],
  ["size", (m) => { m.size = { width: -1, height: 915 }; }],
  ["size", (m) => { delete m.size; }],
  ["dpr", (m) => { m.dpr = 0; }],
  ["navigation", (m) => { m.navigation = "swipe"; }],
  ["controls", (m) => { m.controls = ["back", "teleport", "home", "recents"]; }],
  ["controls", (m) => { m.controls = ["back", "back", "home", "recents"]; }],
  ["controls", (m) => { m.controls = ["back", "home"]; }],
  ["controls", (m) => { m.navigation = "none"; }],
  ["events", (m) => { m.events = ["suspend", "explode"]; }],
  ["events", (m) => { m.events = "suspend"; }],
  ["skin", (m) => { m.skin = "../escape"; }],
];
await check("a bad field is rejected with the named error and the field's name", () => {
  for (const [field, mutate] of bad) {
    const document = good();
    mutate(document);
    const result = parseProfile("android-phone", document);
    assert(result.ok === false, `${field}: the manifest was accepted`);
    assert(result.code === "TESSERA_STUDIO_PROFILE", `${field}: the code is ${result.code}`);
    assert(result.reason.includes(`\`${field}\``) || result.reason.includes(field), `${field}: the reason does not name it: ${result.reason}`);
  }
  assert(parseProfile("x", []).ok === false && parseProfile("x", null).ok === false, "a non-object was accepted");
});

await check("the loader keeps what validates and reports a missing and a broken manifest", async () => {
  const broken = SPEC_SET[1];
  const missing = SPEC_SET[3];
  const fetchStub = async (url) => {
    const id = url.split("/")[2];
    if (id === missing) {
      return { ok: false, status: 404, text: async () => "" };
    }
    if (id === broken) {
      return { ok: true, text: async () => "{ not json" };
    }
    return { ok: true, text: async () => readFileSync(resolve(PROFILES, id, "profile.json"), "utf8") };
  };
  const loaded = await loadBundledProfiles(fetchStub);
  assert(loaded.profiles.length === SPEC_SET.length - 2, `${loaded.profiles.length} profiles loaded`);
  assert(
    loaded.rejected.map((entry) => entry.id).sort().join(",") === [broken, missing].sort().join(","),
    "the rejections do not name the two profiles",
  );
  assert(loaded.rejected.every((entry) => entry.code === "TESSERA_STUDIO_PROFILE" && entry.reason.length > 0), "a rejection lacks its code or reason");
});

const profileOf = (id) => parseProfile(id, manifest(id)).profile;
const controls = { viewport: "320x240", dpr: "1", theme: "system", foldPosture: "", skin: "", profile: "" };

await check("switching device restarts by default and applies size, DPR, skin and fold", () => {
  const next = profileOf("android-phone");
  const result = deviceSwitch(null, next, controls, [], false);
  assert(result.restart === true, "the default switch did not restart");
  assert(result.controls.viewport === "412x915" && result.controls.dpr === "2.625", `size/DPR: ${result.controls.viewport} ${result.controls.dpr}`);
  assert(result.controls.skin === "generic-phone-android" && result.controls.profile === "android-phone", "skin or profile not applied");
  assert(deviceSwitch(null, profileOf("foldable"), controls, [], false).controls.foldPosture === "flat", "a foldable did not open flat");
  assert(deviceSwitch(null, profileOf("ios-phone"), { ...controls, foldPosture: "book" }, [], false).controls.foldPosture === "", "an iPhone kept a hinge");
  const skinned = deviceSwitch(null, next, controls, [{ id: "generic-phone-android", viewer: { width: 396, height: 891 } }], false);
  assert(skinned.controls.viewport === "396x891", "a loaded skin's viewer rectangle is not the size applied");
});

await check("keep-state applies in place only inside one os; a different os always restarts", () => {
  const android = profileOf("android-phone");
  assert(deviceSwitch(android, profileOf("android-tablet"), controls, [], true).restart === false, "same os + keep state restarted");
  assert(deviceSwitch(android, profileOf("android-tablet"), controls, [], false).restart === true, "same os without the setting kept state");
  assert(deviceSwitch(android, profileOf("ios-phone"), controls, [], true).restart === true, "a change of os kept state");
  assert(deviceSwitch(null, android, controls, [], true).restart === true, "the first device kept state it does not have");
  assert(mayKeepState(android, customProfile({ width: 500, height: 500 }, 1, android)) === true, "a custom size does not inherit the os");
  assert(customProfile({ width: 500, height: 500 }, 1, android).controls.length === 0, "a custom size offers buttons");
});

const BASE = { source: "x", file: "ui/Preview.tsx", width: 320, height: 240, dpr: 1, theme: "system", foldPosture: "", skin: "" };

await check("a pre-existing link (no profile key) decodes unchanged", async () => {
  // The envelope a pre-profile page wrote: v, s, w, h, d, t and nothing about a device.
  const legacy = Buffer.from(JSON.stringify({ v: 1, s: "export const a = 1;", w: 390, h: 844, d: 2, t: "dark" })).toString("base64url");
  const decoded = await share.decodeShare("u" + legacy);
  assert(decoded.width === 390 && decoded.height === 844 && decoded.dpr === 2 && decoded.theme === "dark", "the legacy fields changed");
  assert(decoded.profile === "" && decoded.skin === "" && decoded.foldPosture === "", "a legacy link grew a device");
  const withSkin = Buffer.from(JSON.stringify({ v: 1, s: "x", w: 320, h: 240, d: 1, t: "system", k: "generic-tablet", p: "book" })).toString("base64url");
  const skinned = await share.decodeShare("u" + withSkin);
  assert(skinned.skin === "generic-tablet" && skinned.foldPosture === "book" && skinned.profile === "", "an old skin/fold link changed");
  const raw = await share.encodeShare({ ...BASE, profile: "" }, { compress: false });
  const json = JSON.parse(Buffer.from(raw.slice(1), "base64url").toString("utf8"));
  assert(!("n" in json), "a link with no profile carries an `n` key");
});

await check("a profile name round-trips and widens only the size and DPR it owns", async () => {
  for (const id of SPEC_SET) {
    const [width, height, dpr] = state.PROFILE_GEOMETRY[id];
    const input = { ...BASE, width, height, dpr, profile: id };
    const decoded = await share.decodeShare(await share.encodeShare(input));
    assert(decoded.profile === id && decoded.width === width && decoded.height === height && decoded.dpr === dpr, `${id} did not round-trip`);
  }
  let refused = 0;
  for (const bogus of [
    { ...BASE, width: 333 },
    { ...BASE, width: 412, height: 915, profile: "ios-phone" },
    { ...BASE, profile: "pixel-9" },
    { ...BASE, dpr: 2.625 },
    { ...BASE, width: 99999, profile: "custom" },
  ]) {
    try {
      state.normalizeState(bogus);
    } catch (error) {
      assert(error.code === "invalid", `a bogus state failed with ${error.code}`);
      refused += 1;
    }
  }
  assert(refused === 5, `${refused} of 5 bogus states were refused`);
  const custom = await share.decodeShare(await share.encodeShare({ ...BASE, width: 500, height: 777, profile: "custom" }));
  assert(custom.profile === "custom" && custom.width === 500 && custom.height === 777, "a custom size did not round-trip");
});

process.stdout.write(failures === 0 ? "\nprofile checks: all passed\n" : `\nprofile checks: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
