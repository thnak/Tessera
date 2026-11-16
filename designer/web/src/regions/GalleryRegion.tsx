// SPDX-License-Identifier: MIT

// The sample gallery (P5, issue #169): a strip of buttons above the workspace, populated from
// `samples.json` when it is first opened.
//
// **Lazy is the acceptance, not an optimization.** The manifest must not be on the first-paint path, or
// P6's `cold_engine_ready_ms` would pay for a gallery most visitors never open. So this component fetches
// nothing until it is opened: the fetch lives in an effect gated on `open`, once per page load, and the
// manifest is cached in a ref rather than in state (a second open must not refetch).
//
// The buttons and their labels are React's; the *fetch* is `lib/gallery.ts`, which has no DOM so the Node
// smoke test can drive it. A per-sample failure is reported in the strip rather than thrown: a gallery that
// silently drops a sample is worse than one that says it could not load it.

import { useEffect, useRef, useState } from "react";
import {
  cachedManifest,
  loadSampleSource,
  titleOf,
  type GalleryCache,
  type LoadedSample,
  type Sample,
} from "../lib/gallery";

/** The gallery's own status line inside the strip. */
type Note = { text: string; warning: boolean } | null;

export function GalleryRegion({
  open,
  onPick,
}: {
  open: boolean;
  onPick: (sample: LoadedSample) => void;
}) {
  // One cache per page load, in a ref: `useState` would re-render on a fetch that must stay invisible.
  const cache = useRef<GalleryCache>({ samples: null });
  const [samples, setSamples] = useState<Sample[] | null>(null);
  const [note, setNote] = useState<Note>(null);
  const [busy, setBusy] = useState<string | null>(null);

  useEffect(() => {
    if (!open || samples !== null) {
      return;
    }
    let live = true;
    void (async () => {
      try {
        const loaded = await cachedManifest(cache.current);
        if (live) {
          setSamples(loaded);
        }
      } catch (error) {
        if (live) {
          setNote({ text: String(error instanceof Error ? error.message : error), warning: true });
        }
      }
    })();
    return () => {
      live = false;
    };
  }, [open, samples]);

  async function pick(sample: Sample): Promise<void> {
    setBusy(sample.path);
    try {
      const text = await loadSampleSource(sample.path);
      onPick({ ...sample, text });
    } catch (error) {
      setNote({ text: String(error instanceof Error ? error.message : error), warning: true });
    } finally {
      setBusy(null);
    }
  }

  return (
    <section className="gallery" id="gallery" aria-label="Sample gallery" hidden={!open}>
      <div className="gallery-list" id="gallery-list">
        {(samples ?? []).map((sample) => (
          <button
            key={sample.path}
            type="button"
            className="gallery-item"
            data-path={sample.path}
            disabled={busy === sample.path}
            onClick={() => void pick(sample)}
          >
            {titleOf(sample.path)} <code>{sample.component}</code>
          </button>
        ))}
        {note ? (
          <p className={note.warning ? "gallery-note warning" : "gallery-note"}>{note.text}</p>
        ) : null}
      </div>
    </section>
  );
}
