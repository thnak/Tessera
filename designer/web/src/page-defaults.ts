// SPDX-License-Identifier: MIT

// The page's starting state and the strings that go with it: the built-in sample, the default file name,
// and the two sentences the page shows around a share link.
//
// Split out of `App.tsx` so the component stays a composition of the four regions rather than a file that
// also owns the copy. Nothing here is React; it is data, which is why the smoke test can assert the sample
// without rendering anything.

/** The file name the page compiles under when a link names none. */
export const DEFAULT_FILE = "ui/Preview.tsx";

/**
 * The page's built-in sample: what an empty link loads and what "Load sample" restores on Diagnostics.
 *
 * It deliberately carries **no** `automationId`. The compiler rejects one unless it equals the computed
 * `uiId` (`TESSER_IR_AUTOMATION_ID`), so a startup sample with one would show a compile error before the
 * visitor typed anything. The Scene view's own sample (which needs a clickable id) is in
 * `views/scene-session.ts`, and "Load sample" there loads it plus its three-click script.
 */
export const STARTUP_SAMPLE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

/** The toolbar a page starts from before the fragment or local storage says otherwise. */
export const INITIAL_ENVIRONMENT = Object.freeze({
  viewport: "320x240",
  dpr: "1",
  theme: "system",
  /** The simulated fold posture (issue #408): `""` is no fold, which is the desktop default. */
  foldPosture: "",
});

/**
 * The virtual project file the page's stylesheet textarea is filed under (issue #390).
 *
 * A source's `import "./Preview.css";` resolves to this textarea, so an author can either write the
 * import or not: the sheet applies either way, because they typed it for the screen in front of them.
 * The value is the page's, not the engine's -- the engine only defaults to the same string -- so a
 * future folder IDE (#391) can name a real project path here without touching the module.
 */
export const PREVIEW_STYLESHEET_FILE = "ui/Preview.css";

/**
 * The stylesheet textarea's starting text: **one line, and deliberately not CSS**.
 *
 * The owner's decision 2 makes the built-in sheet the fallback for "no stylesheet at all", and the
 * bundled samples must keep painting exactly what they paint today. A comment is therefore the right
 * starting value: `has_author_stylesheet()` treats a comment-only sheet as "no author CSS", so the
 * built-in sheet stays in force, and a visitor who wants to start writing CSS has a line to replace
 * instead of a blank box that looks broken.
 */
export const STARTUP_STYLESHEET = "/* Author CSS for this screen, applied as ui/Preview.css. */\n";
