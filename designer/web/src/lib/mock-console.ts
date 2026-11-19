// SPDX-License-Identifier: MIT

// The React side of the mock host (issue #434, ADR-055): one hook that owns a `createMockHost` for the
// opened project and publishes its statuses and trace to the dock.
//
// **Lazy by construction.** Nothing here imports the host, the worker handle or the transpiler at module
// load: they are dynamic `import()`s made the first time the dock is open *and* the project has a mock
// file (spec section 2.4), so a project without mocks downloads nothing extra and the cold path is
// unchanged. esbuild-wasm is a further `import()` inside `mock/transpile.js`.
//
// **No claim about the preview.** The engine has no service seam (ADR-055 decision 9), so this hook only
// serves calls it is given (the dev hook below, and later the viewer's `mock.call`). It never sends a
// response anywhere: the table it builds is what a future `mock.responses` carries.
//
// **The service manifest** (`ui/services.json`) has no spec yet, so `readServiceManifest` returns `null`
// and every mock is `unbound`, which is exactly what spec section 5.1 says to do until it lands.

import { useCallback, useEffect, useRef, useState } from "react";
import type { ProjectFile } from "./project";

export type MockBinding = "bound" | "unbound" | "error";
export type CallState = "default" | "edited" | "unmocked" | "error";

export interface MockDiagnostic {
  code: string;
  message: string;
  severity: "Error" | "Warn";
  file?: string;
  line?: number;
  column?: number;
}

export interface MockStatus {
  path: string;
  version: number;
  status: MockBinding;
  reason: string;
  interface: string;
  methods: string[];
  diagnostics: MockDiagnostic[];
  disabled: boolean;
  calls: number;
}

export interface CallEntry {
  seq: number;
  interface: string;
  method: string;
  args: unknown[];
  response: Record<string, unknown> | null;
  state: CallState;
  simTimeMs: number;
  delayMs: number | null;
}

export interface ServiceManifest {
  interfaces: Record<string, { methods: string[] }>;
}

/** What the page can say about the mocks right now. */
export interface MockConsole {
  phase: "idle" | "loading" | "ready" | "failed";
  /** A load failure that is not a per-file diagnostic (the transpiler could not be fetched). */
  failure: string;
  statuses: MockStatus[];
  entries: CallEntry[];
  traceStopped: string | null;
  enabled: boolean;
  manifest: ServiceManifest | null;
  reset: () => void;
  restart: (path: string) => void;
  setEnabled: (value: boolean) => void;
  clearTrace: () => void;
  /** Returns a message when the edit is refused, `null` when it was applied. */
  editResponse: (seq: number, text: string) => string | null;
  downloadTrace: () => void;
}

/** `ui/services.json` has no spec (ADR-055 decision 9 (i)), so no project has a readable manifest. */
export function readServiceManifest(): ServiceManifest | null {
  return null;
}

// The host is typed loosely on purpose: it is plain JavaScript shared with the Node gates.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
type Host = any;

const COMPILE_TIMEOUT_MS = 2000;

async function createHost(): Promise<Host> {
  const [{ createMockHost }, { browserWorker }, { compileMock }, wasm] = await Promise.all([
    import("../../mock/host.js"),
    import("../../mock/worker-handle.js"),
    import("../../mock/transpile.js"),
    import("esbuild-wasm/esbuild.wasm?url"),
  ]);
  const wasmURL = wasm.default as string;
  return createMockHost({
    createWorker: browserWorker,
    compile: (text: string, path: string) =>
      Promise.race([
        compileMock(text, path, { wasmURL }),
        new Promise<never>((_, reject) =>
          setTimeout(
            () => reject({ message: "The transpiler did not finish in time.", line: 1, column: 1 }),
            COMPILE_TIMEOUT_MS,
          ),
        ),
      ]),
    seed: 1,
  });
}

/**
 * @param active whether the dock is open: the host starts only then (spec section 2.4)
 * @param files the project's files; the `mock` ones are compiled
 * @param textOf the edited text of a path
 * @param revision bumped when a mock buffer changes, so the worker is recreated (spec section 4.3)
 */
export function useMockConsole(
  active: boolean,
  files: readonly ProjectFile[],
  textOf: (path: string) => string,
  revision: number,
): MockConsole {
  const hostRef = useRef<Host | null>(null);
  const [phase, setPhase] = useState<MockConsole["phase"]>("idle");
  const [failure, setFailure] = useState("");
  const [, setTick] = useState(0);
  const [manifest, setManifest] = useState<ServiceManifest | null>(readServiceManifest);

  const sources = files
    .filter((file) => file.kind === "mock")
    .map((file) => ({ path: file.path, text: textOf(file.path) }));
  // The effect's identity: the set of mock files and their text, compared by value.
  const key = sources.map((source) => `${source.path}\u0000${source.text}`).join("\u0001") + `#${revision}`;
  const sourcesRef = useRef(sources);
  sourcesRef.current = sources;
  const versionRef = useRef(0);

  useEffect(() => {
    if (!active || sourcesRef.current.length === 0) {
      return;
    }
    let cancelled = false;
    // A short pause so a burst of keystrokes in a mock compiles once.
    const timer = setTimeout(async () => {
      setPhase("loading");
      try {
        if (hostRef.current === null) {
          const host = await createHost();
          if (cancelled) {
            host.dispose();
            return;
          }
          host.subscribe(() => setTick((value) => value + 1));
          hostRef.current = host;
        }
        versionRef.current += 1;
        const version = versionRef.current;
        await hostRef.current.load(
          sourcesRef.current.map((source) => ({ ...source, version })),
          manifest,
        );
        if (!cancelled) {
          setFailure("");
          setPhase("ready");
        }
      } catch (cause) {
        if (!cancelled) {
          setFailure(cause instanceof Error ? cause.message : String(cause));
          setPhase("failed");
        }
      }
    }, 250);
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [active, key, manifest]);

  useEffect(
    () => () => {
      hostRef.current?.dispose();
      hostRef.current = null;
    },
    [],
  );

  // A development-only door for driving the host from the browser console and the Playwright check: the
  // service manifest has no spec and the engine has no seam, so there is nothing else to produce a bound
  // mock or a call. Not present in a production build.
  useEffect(() => {
    if (!import.meta.env.DEV) {
      return;
    }
    const door = {
      setManifest: (value: ServiceManifest | null) => setManifest(value),
      call: (iface: string, method: string, args: unknown[], simTimeMs = 0) =>
        hostRef.current?.call(iface, method, args, simTimeMs),
    };
    (window as unknown as { __tesseraMock?: typeof door }).__tesseraMock = door;
    return () => {
      delete (window as unknown as { __tesseraMock?: typeof door }).__tesseraMock;
    };
  }, []);

  const host = hostRef.current;
  const trace = host === null ? null : host.trace();
  const downloadTrace = useCallback(() => {
    const current = hostRef.current;
    if (current === null) {
      return;
    }
    import("../../mock/trace.js").then(({ toDownload }) => {
      const blob = new Blob([JSON.stringify(toDownload(current.trace()), null, 2)], {
        type: "application/json",
      });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = "mock-trace.json";
      anchor.click();
      URL.revokeObjectURL(url);
    });
  }, []);

  return {
    phase,
    failure,
    statuses: host === null ? [] : host.statuses(),
    entries: trace === null ? [] : [...trace.entries],
    traceStopped: trace === null ? null : trace.stopped,
    enabled: host === null ? true : host.enabled,
    manifest,
    reset: () => host?.reset(),
    restart: (path) => void host?.restart(path),
    setEnabled: (value) => host?.setEnabled(value),
    clearTrace: () => host?.clearTrace(),
    editResponse: (seq, text) => {
      const refused = host?.editResponse(seq, text);
      return refused ? `${refused.code}: ${refused.message}` : null;
    },
    downloadTrace,
  };
}
