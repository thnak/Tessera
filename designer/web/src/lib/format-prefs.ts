// SPDX-License-Identifier: MIT

// The format-on-save setting (#577, ADR-061 question 3: on by default, with a setting to turn it off).
// Kept DOM-free and tolerant like the other local preferences: a storage that throws means the default.

const KEY = "tessera.studio.formatOnSave";

/** True unless the person turned format-on-save off. */
export function formatOnSave(storage: Pick<Storage, "getItem"> | null = safeStorage()): boolean {
  try {
    return storage?.getItem(KEY) !== "off";
  } catch {
    return true;
  }
}

export function setFormatOnSave(on: boolean, storage: Pick<Storage, "setItem" | "removeItem"> | null = safeStorage()): void {
  try {
    if (on) {
      storage?.removeItem(KEY);
    } else {
      storage?.setItem(KEY, "off");
    }
  } catch {
    // A sandboxed or full storage: the setting just does not persist.
  }
}

function safeStorage(): Storage | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}
