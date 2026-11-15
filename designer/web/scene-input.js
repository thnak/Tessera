// SPDX-License-Identifier: MIT

// The input-script half of the scene-snapshot preview page: the `click <automationId>` textarea, the
// "Send clicks" run, and the state-cell table it fills (issue #168, P4 of
// docs/design/reference-equantic-ui.md section 7.4).
//
// It is a module of its own because P5 (#169) gave the same page an environment toolbar, a sample
// gallery and a share link: the two halves share only the page's one text-call helper, and keeping
// both in `scene.js` pushed that file past the soft source limit. The seam is deliberately narrow --
// this module never touches the Wasm heap, the module factory or `tessera_free`. It asks `request`
// for the document text, so the page remains the only owner of every allocation and of its matching
// free.
//
// The document is the one tools/codegen/scene_interaction.hpp defines, and
// tests/parity/scene_input_tests.cpp is what keeps the shape honest:
//
//   {"inputVersion":1,
//    "initial":[{"name":"state::count","kind":"i32","value":{"value":"0"}}, ...],
//    "steps":[{"step":0,"kind":"click","automationId":"101","found":true,"dispatched":true,
//              "ran":["handler::1"],"unrun":[],"state":[...]}, ...]}
//
// or `{"error":"..."}` when the script or the source produced nothing to click.

/** The textContent-only element builder the page and this pane share. */
export function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined) {
    node.textContent = text;
  }
  return node;
}

// One state cell as a readable row: the cell id, then its value. A list cell keeps its brackets so a
// snapshot read by eye matches the document's own shape.
function describeCell(cell) {
  const value = cell.value;
  if (value === undefined || value === null) {
    return '(no value)';
  }
  if (Array.isArray(value)) {
    return '[' + value.map((item) => String(item.value ?? '')).join(', ') + ']';
  }
  return String(value.value ?? '');
}

function renderCells(cells, setStatus, snapshot) {
  cells.replaceChildren();
  const initial = Array.isArray(snapshot.initial) ? snapshot.initial : [];
  const steps = Array.isArray(snapshot.steps) ? snapshot.steps : [];
  if (initial.length === 0 && steps.length === 0) {
    setStatus('no state cells', 'warning');
    return;
  }
  // The initial state first, then every step's cells, so the sequence is visible rather than only the
  // final value: a preview that dropped intermediate state still ends at the same place in a simple
  // counter, and this is what makes that difference readable.
  const addRow = (label, state, extra) => {
    const row = element('li', extra ? 'cell step' : 'cell');
    row.append(element('span', 'cell-when', label));
    if (state.length === 0) {
      row.append(element('span', 'cell-name', '(no cells)'));
    }
    for (const cell of state) {
      const pair = element('span', 'cell-pair');
      pair.append(
        element('span', 'cell-name', String(cell.name ?? '')),
        element('span', 'cell-value', describeCell(cell))
      );
      row.append(pair);
    }
    cells.append(row);
  };
  addRow('initial', initial, false);
  for (const step of steps) {
    const label = `click ${step.automationId}`;
    const note = step.found
      ? step.dispatched
        ? ''
        : ' (not dispatched)'
      : ' (no such node)';
    addRow(label + note, Array.isArray(step.state) ? step.state : [], true);
  }
  const last = steps.length > 0 ? steps[steps.length - 1] : null;
  if (last === null) {
    setStatus(`${initial.length} cell(s) at their initial value`, 'ok');
    return;
  }
  const ran = (last.ran || []).length;
  const unrun = (last.unrun || []).length;
  const parts = [`${steps.length} step(s)`, `${ran} handler run`];
  if (unrun > 0) {
    parts.push(`${unrun} handler not run`);
  }
  setStatus(parts.join(', '), unrun > 0 ? 'warning' : 'ok');
}

/**
 * Wires the input pane into a page.
 *
 * `options.request(name, strings)` returns the module's document text for an export; `file()` is the
 * source path the current sample is laid out under (the same one the node list used, or a click would
 * target ids from a different document); `setStatus(text, kind)` is the pane's own status line.
 */
export function mountInputScript(options) {
  const { source, script, button, cells, file, request, setStatus } = options;

  function showProblem(heading, detail) {
    cells.replaceChildren();
    const box = element('div', 'unavailable');
    box.append(element('h3', null, heading), element('p', null, detail));
    cells.append(box);
    setStatus('failed', 'warning');
  }

  async function run() {
    button.disabled = true;
    setStatus('clicking', null);
    try {
      const json = await request('_tessera_input_json', [
        source.value,
        file(),
        '',
        script.value,
      ]);
      const snapshot = JSON.parse(json);
      if (snapshot.error) {
        // A malformed script, or a source that produced no scene: reported as such, never as a
        // successful run with no clicks.
        showProblem('No input result', String(snapshot.error));
        return;
      }
      renderCells(cells, setStatus, snapshot);
    } catch (error) {
      showProblem('Input failed', String(error));
    } finally {
      button.disabled = false;
    }
  }

  // Appending a node's id to the script is what makes the page usable without a hash table in the
  // reader's head: the ids are 64-bit numbers, so nobody types them.
  function append(automationId) {
    if (!automationId || automationId === '0') {
      return;
    }
    const line = `click ${automationId}`;
    const current = script.value.trim();
    script.value = current ? `${current}\n${line}\n` : `${line}\n`;
    setStatus(`added ${line}`, 'ok');
  }

  // Called when the page loads a different source: the cells on screen describe the document that was
  // evaluated, so leaving them up would show another sample's state beside this one's node list.
  function reset() {
    cells.replaceChildren();
    setStatus('no clicks yet', null);
  }

  button.addEventListener('click', () => {
    void run();
  });

  return { run, append, reset };
}
