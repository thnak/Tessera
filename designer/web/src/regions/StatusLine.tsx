// SPDX-License-Identifier: MIT

// Region 4 of `designer-studio-v1.md` section 3: the **status line**.
//
// The spec's status line is richer than this page's (engine id, backend, simulation state, diagnostics
// count, frame time) because the simulation controls it describes (#408, #171) are not part of #392 or this
// conversion. What the page has today, and what this region keeps, is the build id and the per-module
// provenance sentence -- which is the part that answers "is the URL serving the build I pushed?" (D-S5's
// acceptance).
//
// That is a real region rather than a paragraph because the layout has to keep it at the bottom of the
// viewport whatever a pane does, and because #408/#171 will fill it from state React already owns.

/** The page footer: the build's provenance. */
export function StatusLine() {
  return (
    <footer className="page-footer">
      <p>
        Part of <a href="https://github.com/thnak/Tessera">Tessera</a>. The three panes are three Wasm
        modules built from this repository: <code>tessera_diagnostics</code> (the codegen CLI&rsquo;s front
        end), <code>tessera_scene_preview</code> (the evaluator, stylesheet engine, Yoga layout and input
        router on the null RHI) and <code>tessera_canvas</code> (the same scene painted through WebGPU).
        Each is checked against its native counterpart by a CI gate, byte for byte where the output is a
        document. <code>scene.html</code> and <code>canvas.html</code> are redirect stubs kept for links
        made before the merge.
      </p>
    </footer>
  );
}
