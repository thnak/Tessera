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
  /** Changes whenever handlers, overrides or context change; a store snapshot for `useSyncExternalStore`. */
  version(): number;
  subscribe(listener: () => void): () => void;
}
export function createRegistry(
  records: readonly CommandRecord[],
  options: { context: WorkbenchContext; log?: LogFn; platform?: Platform; overrides?: readonly KeybindingOverride[] },
): Registry;
export function validateRecords(records: readonly unknown[]): string[];
export function checkIntegrity(records: readonly CommandRecord[], handlerIds: Iterable<string>): string[];

export interface MenuEntry {
  kind: "item" | "check" | "radio" | "submenu" | "separator";
  key: string;
  label: string;
  detail?: string;
  mnemonic?: string;
  command?: string;
  args?: CommandArgs;
  chord: string;
  enabled: boolean;
  reason: string;
  checked: boolean;
  description: string;
  items: MenuEntry[];
}
export interface MenuBarMenu {
  id: string;
  title: string;
  mnemonic: string;
  items: MenuEntry[];
}
export interface ProviderEntry {
  label: string;
  args?: CommandArgs;
  detail?: string;
  /** A per-entry check mark (a check item), for a menu whose one command cannot index its clause by argument. */
  checked?: boolean;
}
/** What a context menu was opened on (`panel.tab`: the panel whose tab it is). */
export interface MenuTarget {
  panelId?: string;
}
export type MenuProviders = Record<string, (target?: MenuTarget) => ProviderEntry[]>;
export interface MenuDeps {
  registry: Registry;
  providers?: MenuProviders;
  target?: MenuTarget;
  keymap?: Keymap;
  keyPrefix?: string;
}
export interface ToolbarEntry {
  key: string;
  widget?: string;
  command?: string;
  args?: CommandArgs;
  label?: string;
  icon?: string;
  emphasis?: "primary" | "normal";
  enabled?: boolean;
  reason?: string;
  checked?: boolean | null;
  chord?: string;
  description?: string;
}
export interface MenusDocument {
  version: number;
  menus: { id: string; title: string; mnemonic: string; items: unknown[] }[];
  contexts: Record<string, unknown[]>;
  toolbar: unknown[];
}
export function validateMenus(doc: MenusDocument, records: readonly CommandRecord[]): string[];
export function walkMenus(doc: MenusDocument, visit: (item: Record<string, unknown>, where: string) => void): void;
export function buildMenuBar(doc: MenusDocument, deps: MenuDeps): MenuBarMenu[];
export function buildItems(items: readonly unknown[], deps: MenuDeps & { keyPrefix: string }): MenuEntry[];
export function buildContextMenu(doc: MenusDocument, name: string, deps: MenuDeps, target?: MenuTarget): MenuEntry[];
export function buildToolbar(doc: MenusDocument, deps: MenuDeps): ToolbarEntry[];

export interface MenuNav {
  bar: number;
  open: boolean;
  stack: readonly number[];
}
export type MenuEffect = { type: "run"; item: MenuEntry } | { type: "exit" } | { type: "close-bar" } | { type: "close" } | null;
export const CLOSED: MenuNav;
export function navigate(menus: readonly { items: MenuEntry[]; mnemonic?: string }[], nav: MenuNav, key: string): { nav: MenuNav; handled: boolean; effect: MenuEffect };
export function openByMnemonic(menus: readonly { items: MenuEntry[]; mnemonic?: string }[], letter: string): MenuNav | null;
export function focusedItem(menus: readonly { items: MenuEntry[] }[], nav: MenuNav): MenuEntry | null;

export interface RankItem {
  id: string;
  label: string;
  detail?: string;
  basename?: string;
  chord?: string;
}
export interface Ranked<T extends RankItem = RankItem> {
  item: T;
  score: number;
  positions: number[];
  basenameMatch: boolean;
}
export interface RecentList {
  touch(id: string): void;
  list(): string[];
  indexOf(id: string): number;
  load(saved: unknown): void;
  toJSON(): string[];
}
export function fuzzyMatch(query: string, text: string): { score: number; positions: number[] } | null;
export function rankItems<T extends RankItem>(query: string, items: readonly T[], options?: { recent?: { indexOf(id: string): number }; limit?: number }): Ranked<T>[];
export function createRecentList(limit?: number): RecentList;
export function commandItems(registry: Registry): RankItem[];
export function parseInput(value: string): { mode: "commands" | "line" | "files"; text: string; line?: number };
export function parseLine(text: string, lineCount: number): { ok: true; line: number } | { ok: false; message: string };
export interface ArgChoice {
  label: string;
  value: string | number;
  detail: string;
}
export interface ArgStep {
  name: string;
  kind: "choice" | "number" | "text";
  label: string;
  required: boolean;
  choices?: ArgChoice[];
  hint?: string;
}
export interface ArgFlow {
  record: CommandRecord;
  steps: ArgStep[];
  index: number;
  done: boolean;
  step: ArgStep | null;
  args: CommandArgs;
  answer(value: string | number): ArgFlow;
  answerText(text: string): { ok: true; flow: ArgFlow } | { ok: false; message: string };
  back(): ArgFlow;
}
export function createArgFlow(record: CommandRecord, given?: CommandArgs, providers?: MenuProviders): ArgFlow;
export function needsPrompt(record: CommandRecord, given?: CommandArgs): boolean;

export interface NotificationAction {
  label: string;
  command: string;
  args?: CommandArgs;
}
export interface NotificationInput {
  message: string;
  severity?: "info" | "warning" | "error";
  actions?: NotificationAction[];
  /** Same key inside the dedupe window bumps a counter instead of adding an entry. */
  key?: string;
}
export interface NotificationEntry {
  id: number;
  key: string;
  message: string;
  severity: "info" | "warning" | "error";
  actions: NotificationAction[];
  count: number;
  at: number;
  read: boolean;
  active?: boolean;
  expiresAt?: number | null;
}
export interface NotificationsSnapshot {
  version: number;
  dnd: boolean;
  unread: number;
  queued: number;
  toasts: NotificationEntry[];
  history: NotificationEntry[];
}
export interface Notifications {
  notify(input: NotificationInput): number | null;
  dismiss(id: number): void;
  dismissAll(): void;
  tick(): boolean;
  markRead(): void;
  clearHistory(): void;
  setDoNotDisturb(on: boolean): void;
  snapshot(): NotificationsSnapshot;
  version(): number;
  subscribe(listener: () => void): () => void;
}
export function createNotifications(options?: {
  now?: () => number;
  maxVisible?: number;
  historyLimit?: number;
  dedupeMs?: number;
  timeouts?: { info?: number; warning?: number; actionMinimum?: number };
}): Notifications;
