// SPDX-License-Identifier: MIT

// Loading the bundled skins (issue #171's skins slice; `designer-studio-v1.md` section 5.2).
//
// **Why the fetch is its own module.** The validator (`skins.ts`) is pure over a parsed document, so
// the Node gate can hand it fixtures without a filesystem or a network. This module is the half that
// talks to the site: it names the URLs, fetches the manifests, and turns a missing file, a bad
// status and invalid JSON into the same `SkinRejection` shape the validator returns. Keeping them
// apart is what lets the gate test the rules and the page test the wiring.
//
// **A rejected skin is dropped, not fatal.** Section 5.2 says an invalid skin "is not offered", so
// one bad manifest must not take the picker down: the loader keeps the skins that validate and
// returns the rejections beside them, and the page says which skin is missing rather than showing
// six where the spec names seven.
//
// No DOM, no React: the page and the Node gate both call this.

import {
  SKIN_IDS,
  parseSkin,
  type Skin,
  type SkinRejection,
  type SkinResult,
} from "./skins";

/** The `fetch` shape this module needs, so the Node gate can pass a stub. */
export type SkinFetch = (url: string) => Promise<{
  ok: boolean;
  status?: number;
  text(): Promise<string>;
}>;

/** The URL of a skin's manifest, relative to the page's own base (the site root). */
export function skinManifestUrl(id: string): string {
  return `./skins/${id}/skin.json`;
}

/** The URL of a skin's SVG frame, or `null` when the page draws a generated one. */
export function skinArtUrl(skin: Skin): string | null {
  return skin.art === null ? null : `./skins/${skin.id}/${skin.art}`;
}

function reject(reason: string): SkinRejection {
  return { ok: false, code: "TESSERA_STUDIO_SKIN", reason };
}

/**
 * Loads and validates one bundled skin.
 *
 * A missing manifest and a malformed one are the same outcome for the picker - the skin is not
 * offered - but they are different sentences, because "the site is missing a file" and "the manifest
 * is wrong" send a reader to different places.
 */
export async function loadSkin(
  id: string,
  fetchImpl: SkinFetch = (url) => fetch(url),
): Promise<SkinResult> {
  const url = skinManifestUrl(id);
  let response: Awaited<ReturnType<SkinFetch>>;
  try {
    response = await fetchImpl(url);
  } catch (error) {
    return reject(`the manifest ${url} could not be fetched: ${String(error)}`);
  }
  if (!response.ok) {
    return reject(`the manifest ${url} is missing (HTTP ${response.status ?? "?"})`);
  }
  let text: string;
  try {
    text = await response.text();
  } catch (error) {
    return reject(`the manifest ${url} could not be read: ${String(error)}`);
  }
  let document: unknown;
  try {
    document = JSON.parse(text);
  } catch (error) {
    return reject(`the manifest ${url} is not valid JSON: ${String(error)}`);
  }
  return parseSkin(id, document);
}

/** A rejection with the skin it belongs to, so a banner can name the missing skin. */
export type SkinLoadRejection = SkinRejection & { id: string };

/**
 * Loads every bundled skin, keeping the ones that validate and reporting the ones that do not.
 *
 * The order is `SKIN_IDS`'s, which is the picker's order: a `Promise.all` keeps the results aligned
 * with the ids, so a rejection can be attributed without a second lookup.
 */
export async function loadBundledSkins(
  fetchImpl: SkinFetch = (url) => fetch(url),
): Promise<{ skins: Skin[]; rejected: SkinLoadRejection[] }> {
  const results = await Promise.all(SKIN_IDS.map((id) => loadSkin(id, fetchImpl)));
  const skins: Skin[] = [];
  const rejected: SkinLoadRejection[] = [];
  results.forEach((result, index) => {
    if (result.ok) {
      skins.push(result.skin);
    } else {
      rejected.push({ ...result, id: SKIN_IDS[index] });
    }
  });
  return { skins, rejected };
}
