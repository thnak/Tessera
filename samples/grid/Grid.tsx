// SPDX-License-Identifier: MIT

// Grid and container queries (issue #607): a card grid whose column count follows the width of its
// own container (@container), not the window (@media), with named grid areas for the page frame.
// Browser twin: tools/parity/grid.html; keep the two structures identical.

import "./grid.css";

export function Grid() {
  return (
    <div className="page">
      <header className="top">
        <h1>Grid and container queries</h1>
      </header>
      <nav className="side">
        <a>Overview</a>
        <a>Reports</a>
        <a>Settings</a>
      </nav>
      <main className="main">
        <div className="cards">
          <section className="card">
            <h3>Frames</h3>
            <p>Fixed-step loop, render on demand.</p>
          </section>
          <section className="card">
            <h3>Layout</h3>
            <p>Flexbox through Yoga, a grid subset of its own.</p>
          </section>
          <section className="card">
            <h3>Style</h3>
            <p>Cascade, layers, media and container queries.</p>
          </section>
          <section className="card">
            <h3>Text</h3>
            <p>FreeType shaping with a bounded glyph atlas.</p>
          </section>
        </div>
      </main>
    </div>
  );
}
