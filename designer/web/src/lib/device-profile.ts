// SPDX-License-Identifier: MIT

// The device profile (issue #447; `studio-device-profiles-v1.md` section 2 is normative for every field).
//
// A device profile is **data, like a skin**: one directory per profile under the site's `profiles/`, each a
// `profile.json`, and nothing here evaluates it. It names an operating system, a size, a DPR, the
// navigation style, the hardware controls the device offers and the environment events its panel offers,
// and it references a skin (by directory name) for the art, insets and cutout. The strip of buttons and the
// event panel (#448) are generated from `controls` and `events` and from nothing else.
//
// The validator mirrors `views/skins.ts`: pure over a parsed document, a named error (`TESSERA_STUDIO_PROFILE`)
// whose reason is a sentence for a person, and a rejected profile is **not offered** rather than fatal.
//
// No DOM, no React: the page and the Node gate (`scripts/ci/preview_profiles.mjs`) both import this.

/** The four categories of a skin (section 2's `category`). */
export type ProfileCategory = "mobile" | "tablet" | "foldable" | "desktop";

/** The five operating systems of a skin; a change of `os` always restarts the app (section 3). */
export type ProfileOs = "windows" | "macos" | "linux" | "android" | "ios";

/** How the device navigates (section 2): hardware buttons, the gesture bar, or the desktop window. */
export type ProfileNavigation = "three-button" | "gesture" | "none";

/**
 * The hardware or system buttons a profile may offer, in the order the strip shows them. Each is a
 * normalized event of `designer-protocol-v1.md` section 5.1 with the same wire name.
 */
export const CONTROL_IDS = Object.freeze([
  "back",
  "home",
  "recents",
  "volumeUp",
  "volumeDown",
  "power",
] as const);
export type ControlId = (typeof CONTROL_IDS)[number];

/** The environment events a profile's panel may offer (section 4); wire names of section 5.1. */
export const EVENT_IDS = Object.freeze([
  "suspend",
  "resume",
  "screenOff",
  "screenOn",
  "theme",
  "locale",
  "textScale",
  "orientation",
  "fold",
  "ime",
  "memoryPressure",
  "batterySaver",
  "thermal",
  "processDeath",
] as const);
export type EventId = (typeof EVENT_IDS)[number];

export const PROFILE_CATEGORIES: readonly ProfileCategory[] = ["mobile", "tablet", "foldable", "desktop"];
export const PROFILE_OPERATING_SYSTEMS: readonly ProfileOs[] = ["windows", "macos", "linux", "android", "ios"];
export const PROFILE_NAVIGATIONS: readonly ProfileNavigation[] = ["three-button", "gesture", "none"];

/** The largest edge a profile may declare, in logical pixels. */
export const MAX_PROFILE_EDGE = 8192;

/** The DPR range a profile may declare. */
export const MIN_PROFILE_DPR = 0.5;
export const MAX_PROFILE_DPR = 8;

/** A validated profile. `id` is the directory name, which is the picker's value and the link's key. */
export interface DeviceProfile {
  id: string;
  schemaVersion: 1;
  name: string;
  category: ProfileCategory;
  os: ProfileOs;
  size: { width: number; height: number };
  dpr: number;
  navigation: ProfileNavigation;
  controls: ControlId[];
  events: EventId[];
  /** The skin's directory name, or `null` for a profile that draws no frame. */
  skin: string | null;
}

/** A rejected profile. `code` is the studio's named error; `reason` is a sentence for a person. */
export interface ProfileRejection {
  ok: false;
  code: "TESSERA_STUDIO_PROFILE";
  reason: string;
}

export type ProfileResult = { ok: true; profile: DeviceProfile } | ProfileRejection;

/** The custom entry's id (`PROFILE_IDS` in `preview-state.js`): the ad hoc profile a custom size makes. */
export const CUSTOM_PROFILE_ID = "custom";

function reject(id: string, reason: string): ProfileRejection {
  return { ok: false, code: "TESSERA_STUDIO_PROFILE", reason: `profile \`${id}\`: ${reason}` };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function edge(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value > 0 && value <= MAX_PROFILE_EDGE;
}

/** The subset of `list` named by `value`, in `list`'s order, or the sentence that says why not. */
function pickList<T extends string>(
  field: string,
  value: unknown,
  list: readonly T[],
): { ok: true; items: T[] } | { ok: false; reason: string } {
  if (!Array.isArray(value)) {
    return { ok: false, reason: `\`${field}\` must be an array` };
  }
  const seen = new Set<string>();
  for (const entry of value) {
    if (typeof entry !== "string" || !(list as readonly string[]).includes(entry)) {
      return {
        ok: false,
        reason: `\`${field}\` names \`${String(entry)}\`, which is not one of ${list.join(", ")}`,
      };
    }
    if (seen.has(entry)) {
      return { ok: false, reason: `\`${field}\` names \`${entry}\` twice` };
    }
    seen.add(entry);
  }
  return { ok: true, items: list.filter((item) => seen.has(item)) };
}

/**
 * Validates one parsed `profile.json`.
 *
 * `id` is the directory name the manifest was read from. Every rejection names the field, so a person
 * fixing a manifest does not have to guess which of its eleven fields the validator disliked.
 */
export function parseProfile(id: string, document: unknown): ProfileResult {
  if (!isRecord(document)) {
    return reject(id, "the manifest is not a JSON object");
  }
  if (document.schemaVersion !== 1) {
    return reject(id, `\`schemaVersion\` is ${JSON.stringify(document.schemaVersion)}; this page reads 1`);
  }
  if (typeof document.name !== "string" || document.name.trim() === "" || document.name.length > 80) {
    return reject(id, "`name` must be a non-empty string of at most 80 characters");
  }
  if (!PROFILE_CATEGORIES.includes(document.category as ProfileCategory)) {
    return reject(id, `\`category\` must be one of ${PROFILE_CATEGORIES.join(", ")}`);
  }
  if (!PROFILE_OPERATING_SYSTEMS.includes(document.os as ProfileOs)) {
    return reject(id, `\`os\` must be one of ${PROFILE_OPERATING_SYSTEMS.join(", ")}`);
  }
  const size = document.size;
  if (!isRecord(size) || !edge(size.width) || !edge(size.height)) {
    return reject(id, `\`size\` must have integer \`width\` and \`height\` from 1 to ${MAX_PROFILE_EDGE}`);
  }
  const dpr = document.dpr;
  if (typeof dpr !== "number" || !Number.isFinite(dpr) || dpr < MIN_PROFILE_DPR || dpr > MAX_PROFILE_DPR) {
    return reject(id, `\`dpr\` must be a number from ${MIN_PROFILE_DPR} to ${MAX_PROFILE_DPR}`);
  }
  if (!PROFILE_NAVIGATIONS.includes(document.navigation as ProfileNavigation)) {
    return reject(id, `\`navigation\` must be one of ${PROFILE_NAVIGATIONS.join(", ")}`);
  }
  const controls = pickList("controls", document.controls, CONTROL_IDS);
  if (!controls.ok) {
    return reject(id, controls.reason);
  }
  const events = pickList("events", document.events, EVENT_IDS);
  if (!events.ok) {
    return reject(id, events.reason);
  }
  // A navigation style and its buttons must agree: a desktop window has no Back or Home button, and a
  // three-button device without them would show a strip with nothing to press.
  if (document.navigation === "none" && controls.items.length > 0) {
    return reject(id, "`navigation` is `none`, so `controls` must be empty");
  }
  if (document.navigation === "three-button") {
    for (const required of ["back", "home", "recents"] as const) {
      if (!controls.items.includes(required)) {
        return reject(id, `\`navigation\` is \`three-button\`, so \`controls\` must include \`${required}\``);
      }
    }
  }
  const skin = document.skin;
  if (skin !== undefined && (typeof skin !== "string" || !/^[a-z0-9][a-z0-9-]*$/.test(skin))) {
    return reject(id, "`skin` must be a skin directory name");
  }
  return {
    ok: true,
    profile: {
      id,
      schemaVersion: 1,
      name: document.name.trim(),
      category: document.category as ProfileCategory,
      os: document.os as ProfileOs,
      size: { width: size.width, height: size.height },
      dpr,
      navigation: document.navigation as ProfileNavigation,
      controls: controls.items,
      events: events.items,
      skin: typeof skin === "string" ? skin : null,
    },
  };
}

/**
 * The ad hoc profile a custom size makes (section 3's "custom-size entry"). It keeps the operating
 * system of the profile it was made from so a keep-state switch still has an `os` to compare, offers no
 * hardware buttons, and offers the environment events that need no particular device.
 */
export function customProfile(
  size: { width: number; height: number },
  dpr: number,
  from: DeviceProfile | null,
): DeviceProfile {
  return {
    id: CUSTOM_PROFILE_ID,
    schemaVersion: 1,
    name: "Custom size",
    category: from?.category ?? "desktop",
    os: from?.os ?? "windows",
    size,
    dpr,
    navigation: "none",
    controls: [],
    events: ["suspend", "resume", "theme", "locale", "textScale", "orientation", "memoryPressure"],
    skin: null,
  };
}

/**
 * Whether a switch from `from` to `to` may keep the running app's state (section 3).
 *
 * Only a change inside one `os` may: a different platform changes the controls and the initial
 * environment, which is a different launch. Without a previous profile there is nothing to keep.
 */
export function mayKeepState(from: DeviceProfile | null, to: DeviceProfile): boolean {
  return from !== null && from.os === to.os;
}
