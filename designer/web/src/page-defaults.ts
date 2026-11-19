// SPDX-License-Identifier: MIT

// The page's starting state and the strings that go with it: the built-in sample, the default file name,
// and the sentences the page shows around it.
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
  /** The simulated device skin (issue #171's skins slice): `""` is no frame, the pre-#171 page. */
  skin: "",
  /** The device profile (issue #447): `""` is none, the pre-profile page. */
  profile: "",
});
