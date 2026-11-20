// SPDX-License-Identifier: MIT

// The Studio's first sample: what a React + Vite developer would write, written the same way.
// Plain HTML-style elements (h1, p, ul, a, button, input), a global stylesheet next to a CSS module,
// design tokens as custom properties, hover / focus / active states, a responsive layout, a
// conditional child, controlled input and state.
// Its browser twin is `tools/parity/showcase.html`; keep the two structures identical.

import { useState } from "tessera";
import "./app.css";
import styles from "./showcase.module.css";

export function Showcase() {
  const [count, setCount] = useState<int>(0);
  const [name, setName] = useState<string>("");
  const [tab, setTab] = useState<int>(0);

  return (
    <div className="app">
      <header className={styles.hero}>
        <p className={styles.eyebrow}>Tessera</p>
        <h1>React-style UI, native speed</h1>
        <p className={styles.lead}>
          Write TSX and CSS the way you already do. Tessera compiles it to C++ and draws it with its own
          renderer, so the page below is the same code in the Studio and in a shipped app.
        </p>
        <div className={styles.actions}>
          <button className={styles.primary} onClick={() => setCount(count + 1)}>
            Get started
          </button>
          <a className={styles.link}>Read the docs</a>
        </div>
      </header>

      <section className={styles.stats}>
        <div className={styles.stat}>
          <p className={styles.statValue}>16 ms</p>
          <p className={styles.statLabel}>Frame budget</p>
        </div>
        <div className={styles.stat}>
          <p className={styles.statValue}>0 bytes</p>
          <p className={styles.statLabel}>Runtime JS</p>
        </div>
        <div className={styles.stat}>
          <p className={styles.statValue}>{count}</p>
          <p className={styles.statLabel}>Clicks so far</p>
        </div>
      </section>

      <section className={styles.panel}>
        <div className={styles.tabs}>
          <button className={tab === 0 ? styles.tabOn : styles.tab} onClick={() => setTab(0)}>
            Overview
          </button>
          <button className={tab === 1 ? styles.tabOn : styles.tab} onClick={() => setTab(1)}>
            Try it
          </button>
        </div>
        {tab === 0 && (
          <ol className={styles.steps}>
            <li className={styles.step}>
              <span className={styles.stepIndex}>1</span>
              <div className={styles.stepBody}>
                <h3>Write</h3>
                <p>Components, props, hooks and a stylesheet, as in React.</p>
              </div>
            </li>
            <li className={styles.step}>
              <span className={styles.stepIndex}>2</span>
              <div className={styles.stepBody}>
                <h3>Compile</h3>
                <p>TSX becomes IR, then C++ with no JavaScript at run time.</p>
              </div>
            </li>
            <li className={styles.step}>
              <span className={styles.stepIndex}>3</span>
              <div className={styles.stepBody}>
                <h3>Ship</h3>
                <p>One engine draws the Studio preview and your app.</p>
              </div>
            </li>
          </ol>
        )}
        {tab === 1 && (
          <div className={styles.form}>
            <label>Your name</label>
            <input aria-label="Your name" value={name} onChange={(e) => setName(e.value)} />
            {name !== "" && <p className={styles.hello}>Hello, {name}.</p>}
          </div>
        )}
      </section>
    </div>
  );
}
