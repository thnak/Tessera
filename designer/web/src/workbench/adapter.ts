// SPDX-License-Identifier: MIT

// The library-adapter interface (studio-workbench-layout-v1.md sections 1 and 2.4). A dock library is an
// adapter: it renders a `Layout` and reports what the viewer did as an `Operation`. It never owns the layout
// and its own serialization is never persisted, so replacing the library (the exit named in ADR-062) touches
// one file and no stored data.

import type { Layout, Operation } from "./types";

export interface DockAdapterHandle {
  /** Renders a new canonical layout. The adapter must not mount, unmount or re-parent panel content. */
  render(layout: Layout): void;
  /** Moves focus to a panel's tab (after a keyboard move). */
  focusTab(panelId: string): void;
  dispose(): void;
}

export interface DockAdapterOptions {
  /** A gesture became an operation; the owner applies it and calls `render` with the result. */
  onOperation(operation: Operation): void;
}

/** Library JSON is derived from the layout and discarded; round-tripping must be lossless (a W5 gate). */
export interface LayoutCodec<LibraryJson> {
  toLibrary(layout: Layout): LibraryJson;
  fromLibrary(json: LibraryJson, reference: Layout): Layout;
}
