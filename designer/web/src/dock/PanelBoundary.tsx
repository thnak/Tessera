// SPDX-License-Identifier: MIT

import { Component, type ReactNode } from "react";

interface Props {
  panelId: string;
  title: string;
  /** Called after "Reload panel", so the page can re-bind a session to the re-rendered content. */
  onReload?: () => void;
  children: ReactNode;
}

/**
 * Failure isolation (studio-workbench-layout-v1.md section 4.3): a render error in one panel shows "This panel
 * failed" with a **Reload panel** button in that panel only; siblings and the dock are untouched.
 */
export class PanelBoundary extends Component<Props, { failed: boolean; attempt: number }> {
  state = { failed: false, attempt: 0 };

  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true };
  }

  componentDidCatch(error: unknown): void {
    console.error(`the ${this.props.panelId} panel failed:`, error);
  }

  render(): ReactNode {
    if (this.state.failed) {
      return (
        <div className="panel-failed" role="alert" data-panel-failed={this.props.panelId}>
          <p>This panel failed ({this.props.title}).</p>
          <button
            type="button"
            onClick={() => {
              this.setState((s) => ({ failed: false, attempt: s.attempt + 1 }));
              this.props.onReload?.();
            }}
          >
            Reload panel
          </button>
        </div>
      );
    }
    return this.props.children;
  }
}
