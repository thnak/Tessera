// SPDX-License-Identifier: MIT

// Loading the bundled device profiles (issue #447; `studio-device-profiles-v1.md` section 2).
//
// The fetch is its own module for the reason `views/skin-load.ts` is: the validator is pure over a parsed
// document so the Node gate hands it fixtures, and this half names the URLs and turns a missing file, a
// bad status and invalid JSON into the same `ProfileRejection` the validator returns. A rejected profile
// is dropped, not fatal: one bad manifest must not take the picker down.
//
// No DOM, no React.

import { PROFILE_IDS } from "./preview-state";
import { parseProfile, type DeviceProfile, type ProfileRejection, type ProfileResult } from "./device-profile";

/** The `fetch` shape this module needs, so the Node gate can pass a stub. */
export type ProfileFetch = (url: string) => Promise<{
  ok: boolean;
  status?: number;
  text(): Promise<string>;
}>;

/** The bundled ids, in the picker's order: `PROFILE_IDS` without the empty and the `custom` entries. */
export const BUNDLED_PROFILE_IDS: readonly string[] = PROFILE_IDS.filter(
  (id) => id !== "" && id !== "custom",
);

/** The URL of a profile's manifest, relative to the page's own base (the site root). */
export function profileManifestUrl(id: string): string {
  return `./profiles/${id}/profile.json`;
}

function reject(id: string, reason: string): ProfileRejection {
  return { ok: false, code: "TESSERA_STUDIO_PROFILE", reason: `profile \`${id}\`: ${reason}` };
}

/** Loads and validates one bundled profile. */
export async function loadProfile(
  id: string,
  fetchImpl: ProfileFetch = (url) => fetch(url),
): Promise<ProfileResult> {
  const url = profileManifestUrl(id);
  let response: Awaited<ReturnType<ProfileFetch>>;
  try {
    response = await fetchImpl(url);
  } catch (error) {
    return reject(id, `the manifest ${url} could not be fetched: ${String(error)}`);
  }
  if (!response.ok) {
    return reject(id, `the manifest ${url} is missing (HTTP ${response.status ?? "?"})`);
  }
  let document: unknown;
  try {
    document = JSON.parse(await response.text());
  } catch (error) {
    return reject(id, `the manifest ${url} is not valid JSON: ${String(error)}`);
  }
  return parseProfile(id, document);
}

/** A rejection with the profile it belongs to, so a banner can name the missing profile. */
export type ProfileLoadRejection = ProfileRejection & { id: string };

/** Loads every bundled profile, keeping the ones that validate and reporting the ones that do not. */
export async function loadBundledProfiles(
  fetchImpl: ProfileFetch = (url) => fetch(url),
): Promise<{ profiles: DeviceProfile[]; rejected: ProfileLoadRejection[] }> {
  const results = await Promise.all(BUNDLED_PROFILE_IDS.map((id) => loadProfile(id, fetchImpl)));
  const profiles: DeviceProfile[] = [];
  const rejected: ProfileLoadRejection[] = [];
  results.forEach((result, index) => {
    if (result.ok) {
      profiles.push(result.profile);
    } else {
      rejected.push({ ...result, id: BUNDLED_PROFILE_IDS[index] });
    }
  });
  return { profiles, rejected };
}
