// SPDX-License-Identifier: MIT

// The welcome view: what the editor column shows while no project is open (owner feedback on #586: the
// horizontal samples bar is gone, samples live here, in File > Open Sample and in the command palette).
//
// **Lazy is still the acceptance** (issue #169): the manifest is fetched when this view mounts, which is
// only while nothing is open, so a visitor who resumes a project never pays for the list. The fetch and
// the titles are `lib/gallery.ts`; a failure is shown here, never thrown.

import { useEffect, useState } from "react";
import { loadSampleProject, pageManifest, titleOf, type LoadedSample, type Sample } from "../lib/gallery";

export function WelcomeView({
  onPick,
  run,
}: {
  onPick: (sample: LoadedSample) => void;
  /** Runs a command id (the same registry as the menu), so a button here is never a second code path. */
  run: (id: string) => void;
}) {
  const [samples, setSamples] = useState<Sample[] | null>(null);
  const [note, setNote] = useState("");
  const [busy, setBusy] = useState("");

  useEffect(() => {
    let live = true;
    pageManifest().then(
      (loaded) => live && setSamples(loaded),
      (error: unknown) => live && setNote(String(error instanceof Error ? error.message : error)),
    );
    return () => {
      live = false;
    };
  }, []);

  async function pick(sample: Sample): Promise<void> {
    setBusy(sample.path);
    try {
      onPick(await loadSampleProject(sample));
    } catch (error) {
      setNote(String(error instanceof Error ? error.message : error));
    } finally {
      setBusy("");
    }
  }

  return (
    <div className="welcome" id="welcome" role="region" aria-label="Get started">
      <h2 className="welcome-heading">Get started</h2>
      <div className="welcome-actions">
        <button type="button" onClick={() => run("file.openFolder")}>
          Open Folder...
        </button>
        <button type="button" onClick={() => run("file.openZip")}>
          Open Zip...
        </button>
      </div>
      <h3 className="welcome-sub">Samples</h3>
      <ul className="welcome-samples" id="welcome-samples">
        {(samples ?? []).map((sample) => (
          <li key={sample.path}>
            <button
              type="button"
              className="welcome-sample"
              data-path={sample.path}
              disabled={busy === sample.path}
              onClick={() => void pick(sample)}
            >
              {titleOf(sample.path)} <code>{sample.component}</code>
            </button>
          </li>
        ))}
      </ul>
      {note !== "" ? (
        <p className="welcome-note" role="alert">
          {note}
        </p>
      ) : null}
    </div>
  );
}
