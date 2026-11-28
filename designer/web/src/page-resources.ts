// SPDX-License-Identifier: MIT

// The page's fake-resource state (issue #657; `designer-protocol-v1.md` section 5.2).
//
// **What this hook owns.** The last report the engine gave and nothing else: whether a resource is
// loading, refreshing or failed is the engine's fact, so the panel shows the report and never a copy it
// keeps in step. It sends one control through the canvas session and stores the reply; `refresh()` is a
// `status` read, which the page makes after every apply because an apply starts a fresh evaluation whose
// resources open on a fetch in flight.

import { useCallback, useRef, useState } from "react";
import {
  STATUS_REQUEST,
  unavailableReport,
  type ResourceReport,
  type ResourceRequest,
} from "./lib/resource-controls";

/** What the page hands the hook: how to reach the canvas session. */
export interface ResourceDeps {
  /** Delivers one control to the running canvas session. */
  deliver: (request: ResourceRequest) => ResourceReport;
}

export interface ResourceControls {
  /** The engine's last report, or `null` before the first read. */
  report: ResourceReport | null;
  /** Sends one control and keeps the reply. */
  send: (request: ResourceRequest) => ResourceReport;
  /** Reads the report without applying anything (a `status` control). */
  refresh: () => void;
}

export function useResources(deps: ResourceDeps): ResourceControls {
  const [report, setReport] = useState<ResourceReport | null>(null);
  const depsRef = useRef(deps);
  depsRef.current = deps;

  const send = useCallback((request: ResourceRequest): ResourceReport => {
    const reply = depsRef.current.deliver(request);
    setReport(reply);
    return reply;
  }, []);

  const refresh = useCallback(() => {
    const reply = depsRef.current.deliver(STATUS_REQUEST);
    // Before the first apply there is no session: show nothing rather than an error nobody caused.
    setReport(reply.ok ? reply : unavailableReport(""));
  }, []);

  return { report, send, refresh };
}
