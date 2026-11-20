// SPDX-License-Identifier: MIT

// The welcome view: what the editor column shows while no project is open (owner feedback on #586: the
// horizontal samples bar is gone).
//
// **The page bundles no samples** (owner decision: there is no server, so nothing is fetched). A person opens
// a folder or a zip from their computer; to try a sample they clone the repository and open
// `samples/<name>`, or the repository root so library imports resolve (`lib/sample-folder.ts`). A picked
// folder with several samples lists them here and in the Open Sample quick pick.

import { SAMPLES_URL } from "../lib/sample-folder";

export function WelcomeView({
  choices,
  onChoose,
  run,
}: {
  /** The sample entries of the folder just picked, when it holds several; empty otherwise. */
  choices: readonly string[];
  onChoose: (entry: string) => void;
  /** Runs a command id (the same registry as the menu), so a button here is never a second code path. */
  run: (id: string) => void;
}) {
  return (
    <div className="welcome" id="welcome" role="region" aria-label="Get started">
      <h2 className="welcome-heading">Get started</h2>
      <p className="welcome-help" id="welcome-open">
        Open a folder or files from your computer.
      </p>
      <div className="welcome-actions">
        <button type="button" onClick={() => run("file.openFolder")}>
          Open Folder...
        </button>
        <button type="button" onClick={() => run("file.openZip")}>
          Open Zip...
        </button>
      </div>
      <h3 className="welcome-sub">Samples</h3>
      <p className="welcome-help" id="welcome-samples-help">
        Clone the repository, then open <code>samples/&lt;name&gt;</code> (the samples are listed at{" "}
        <a href={SAMPLES_URL} target="_blank" rel="noreferrer">
          {SAMPLES_URL}
        </a>
        ). Open the repository root instead to use a sample that imports the component library
        (<code>packages/tessera-ui</code>).
      </p>
      {choices.length > 0 ? (
        <ul className="welcome-samples" id="welcome-samples">
          {choices.map((path) => (
            <li key={path}>
              <button type="button" className="welcome-sample" data-path={path} onClick={() => onChoose(path)}>
                <code>{path}</code>
              </button>
            </li>
          ))}
        </ul>
      ) : null}
    </div>
  );
}
