// SPDX-License-Identifier: MIT

// The Resources section of the studio's Platform events panel (issue #657; `designer-protocol-v1.md`
// section 5.2 is normative for the wire names and the reply, `pending-ui-v1.md` section 10.4 for what the
// engine does with them).
//
// Everything here is **pure**: the request, the reply, the draft validation and the sentences the panel
// shows. No DOM, no React and no Wasm, so the Node gate (`scripts/ci/preview_resource_controls.mjs`)
// imports the very code the page ships.
//
// The engine owns the state. The page never keeps its own copy of a resource's status: it sends one
// control, reads the report the engine answers with, and shows that. A `status` request reads the report
// without applying anything, which is how the panel learns the opening state after an apply.

/** What a control does to one fake resource (`tessera_canvas_resource`'s `action`). */
export type ResourceAction = "fetch" | "resolve" | "fail" | "status";

/** One control the page sends. `epoch` 0 is the resource's current epoch. */
export interface ResourceRequest {
  action: ResourceAction;
  /** `res::<name>` or the bare name; empty for `status`. */
  resource: string;
  /** The value of a number-typed resource. */
  number: number;
  /** The value of a string-typed resource, and the message of a `fail`. */
  text: string;
  epoch: number;
}

/** One resource of the open component, as the engine reports it. */
export interface ResourceStatus {
  id: string;
  status: string;
  epoch: number;
  error: string;
}

/** One Suspense boundary: whether it shows its fallback, or holds its content through a transition. */
export interface BoundaryStatus {
  id: string;
  fallback: boolean;
  held: boolean;
}

/** The reply to one control (`designer-protocol-v1.md` section 5.2). */
export interface ResourceReport {
  ok: boolean;
  /** The control changed a resource; false for a status read and for a dropped stale completion. */
  accepted: boolean;
  /** A frame is owed. */
  changed: boolean;
  isPending: boolean;
  resources: ResourceStatus[];
  boundaries: BoundaryStatus[];
  /** Present when `ok` is false, or when the page could not make the call. */
  error: string;
}

/** The text fields of a resource row, exactly as typed. */
export interface ResourceDraft {
  value: string;
  epoch: string;
}

/** The report of a call that could not be made (no module, an older build, no session). */
export function unavailableReport(message: string): ResourceReport {
  return {
    ok: false,
    accepted: false,
    changed: false,
    isPending: false,
    resources: [],
    boundaries: [],
    error: message,
  };
}

function asString(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function asNumber(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

/** Parses the engine's one-line report. A document that is not one is an unavailable report, never a throw. */
export function parseReport(text: string): ResourceReport {
  let raw: Record<string, unknown>;
  try {
    raw = JSON.parse(text) as Record<string, unknown>;
  } catch {
    return unavailableReport("the preview engine answered a resource control with a document that is not JSON");
  }
  if (typeof raw !== "object" || raw === null || raw.pendingVersion !== 1) {
    return unavailableReport("the preview engine answered a resource control with an unknown report version");
  }
  const rows = Array.isArray(raw.resources) ? (raw.resources as Record<string, unknown>[]) : [];
  const boundaries = Array.isArray(raw.boundaries) ? (raw.boundaries as Record<string, unknown>[]) : [];
  return {
    ok: raw.ok === true,
    accepted: raw.accepted === true,
    changed: raw.changed === true,
    isPending: raw.isPending === true,
    resources: rows.map((row) => ({
      id: asString(row.id),
      status: asString(row.status),
      epoch: asNumber(row.epoch),
      error: asString(row.error),
    })),
    boundaries: boundaries.map((row) => ({
      id: asString(row.id),
      fallback: row.fallback === true,
      held: row.held === true,
    })),
    error: asString(raw.error),
  };
}

/** The read the panel makes after an apply: nothing is applied and no frame is owed. */
export const STATUS_REQUEST: ResourceRequest = Object.freeze({
  action: "status",
  resource: "",
  number: 0,
  text: "",
  epoch: 0,
});

/** Why a draft cannot be sent, or `null`. An empty epoch means "the current one". */
export function draftError(draft: ResourceDraft): string | null {
  const epoch = draft.epoch.trim();
  if (epoch !== "" && !/^[1-9][0-9]{0,9}$/.test(epoch)) {
    return "The epoch is a whole number from 1, or empty for the current one.";
  }
  return null;
}

/**
 * The request a button sends for `row`. A `resolve` carries the typed value as both the text of a string
 * resource and the number of a number one (the engine reads the one its type needs); `fail` carries it as
 * the message. A `fetch` carries nothing and takes no epoch: it starts the next one.
 */
export function requestFor(action: Exclude<ResourceAction, "status">, resource: string, draft: ResourceDraft): ResourceRequest {
  const value = action === "fetch" ? "" : draft.value;
  const number = action === "resolve" && value.trim() !== "" ? Number(value) : 0;
  const epoch = action === "fetch" || draftError(draft) !== null ? 0 : Number(draft.epoch.trim() || "0");
  return { action, resource, number: Number.isFinite(number) ? number : 0, text: value, epoch };
}

/** The status word the panel shows for one resource, with its epoch and error when it has one. */
export function describeResource(row: ResourceStatus): string {
  const base = `${row.status} (epoch ${row.epoch})`;
  return row.error === "" ? base : `${base}: ${row.error}`;
}

/** What the open component shows right now, in one sentence: fallback, held content, or content. */
export function describePending(report: ResourceReport): string {
  if (report.resources.length === 0) {
    return "This component declares no resource.";
  }
  const fallbacks = report.boundaries.filter((boundary) => boundary.fallback).length;
  const held = report.boundaries.filter((boundary) => boundary.held).length;
  const parts: string[] = [];
  if (fallbacks > 0) {
    parts.push(fallbacks === 1 ? "A boundary shows its fallback." : `${fallbacks} boundaries show their fallback.`);
  }
  if (held > 0) {
    parts.push(held === 1 ? "A boundary holds its content." : `${held} boundaries hold their content.`);
  }
  if (parts.length === 0) {
    parts.push("The content is showing.");
  }
  parts.push(report.isPending ? "isPending is true." : "isPending is false.");
  return parts.join(" ");
}
