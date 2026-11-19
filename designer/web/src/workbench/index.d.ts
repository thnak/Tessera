// SPDX-License-Identifier: MIT

// Type declarations for the workbench-core modules. The modules stay `.js` on purpose, like `share.js` and
// `preview-state.js`: they are pure ES modules that `scripts/ci/preview_workbench*.mjs` imports directly under
// Node, so the gates test the shipped source with no build.

export type ContextValue = boolean | number | string;
export type ContextRecord = Record<string, ContextValue>;

export interface WorkbenchContext {
  snapshot(): ContextRecord;
  get(key: string): ContextValue;
  /** Replaces values and notifies once, only when something changed. Returns whether it did. */
  set(patch: ContextRecord): boolean;
  subscribe(listener: (values: ContextRecord) => void): () => void;
}
export function createContext(initial?: ContextRecord): WorkbenchContext;
export const CONTEXT_KEYS: readonly string[];
export function isKnownContextKey(key: string): boolean;

export function evaluate(clause: string, context: ContextRecord): boolean;
export function clauseError(clause: string): string | null;
export function clauseKeys(clause: string): string[];
export function specificityOf(clause: string | null | undefined): number;
export function parseClause(clause: string): { ok: true; specificity: number; keys: string[] } | { ok: false; error: string };

export interface JsonSchemaProperty {
  type?: "string" | "integer" | "number" | "boolean";
  enum?: readonly (string | number)[];
  minimum?: number;
  maximum?: number;
  pattern?: string;
  /** Names a page-registered choice provider (`recent`, `skins`, ...); the palette lists its entries. */
  "x-provider"?: string;
}
export interface ArgsSchema {
  type: "object";
  properties: Record<string, JsonSchemaProperty>;
  required?: readonly string[];
}

export interface CommandRecord {
  id: string;
  title: string;
  titleShort?: string;
  category: string;
  description?: string;
  keybinding?: { default: string; also?: readonly string[]; when?: string };
  enabled?: string;
  visible?: string;
  checked?: string;
  args?: ArgsSchema;
  services?: readonly string[];
  surfaces?: readonly string[];
  note?: string;
  status: "live" | "reserved" | "removed";
  replacedBy?: string | null;
}

export type CommandArgs = Record<string, string | number | boolean>;
export type RunResult = { ok: true; value?: unknown } | { ok: false; code: string; message: string };
export type CommandHandler = (args: CommandArgs, meta: { id: string }) => unknown | Promise<unknown>;

export interface CommandState {
  record: CommandRecord;
  runnable: boolean;
  enabled: boolean;
  reason: string;
  visible: boolean;
  checked: boolean | null;
}

export interface KeyEventLike {
  key: string;
  code?: string;
  ctrlKey: boolean;
  altKey: boolean;
  shiftKey: boolean;
  metaKey: boolean;
}
export interface ChordStep {
  ctrl: boolean;
  alt: boolean;
  shift: boolean;
  meta: boolean;
  key: string;
}
export type Resolution =
  | { status: "match"; id: string; pending: ChordStep[] }
  | { status: "pending"; pending: ChordStep[] }
  | { status: "none"; pending: ChordStep[]; ignored?: boolean };

export interface KeybindingOverride {
  key: string;
  command: string;
  when?: string;
}
export interface Keymap {
  resolve(event: KeyEventLike, pending: ChordStep[], context: ContextRecord): Resolution;
  chordFor(id: string): string;
  rejected: { id: string; key: string; reason: string }[];
  conflicts(): { chord: string; commands: { id: string; when: string | null }[] }[];
}
export type Platform = "win" | "mac";
export function createKeymap(options: {
  commands: readonly CommandRecord[];
  overrides?: readonly KeybindingOverride[];
  platform?: Platform;
  isActive?: (id: string) => boolean;
}): Keymap;
export function parseChord(text: string, platform?: Platform): ChordStep[] | null;
export function formatChord(steps: readonly ChordStep[], platform?: Platform): string;
export function chordProblem(text: string): string | null;
export function reservedProblem(text: string): { reserved: string; fallback: string | null } | null;
export function stepOfEvent(event: KeyEventLike): ChordStep;

export type LogFn = (level: "info" | "warn" | "error", event: string, detail?: object) => void;
export interface Registry {
  readonly records: readonly CommandRecord[];
  readonly context: WorkbenchContext;
  readonly platform: Platform;
  get(id: string): CommandRecord | undefined;
  ids(): string[];
  handlerIds(): string[];
  register(id: string, handler: CommandHandler): () => void;
  integrity(): string[];
  describe(id: string, args?: CommandArgs | null): CommandState | null;
  forSurface(surface: string): CommandRecord[];
  run(id: string, args?: CommandArgs): Promise<RunResult>;
  keymap(): Keymap;
  setOverrides(overrides: readonly KeybindingOverride[]): void;
  subscribe(listener: () => void): () => void;
}
export function createRegistry(
  records: readonly CommandRecord[],
  options: { context: WorkbenchContext; log?: LogFn; platform?: Platform; overrides?: readonly KeybindingOverride[] },
): Registry;
export function validateRecords(records: readonly unknown[]): string[];
export function checkIntegrity(records: readonly CommandRecord[], handlerIds: Iterable<string>): string[];
