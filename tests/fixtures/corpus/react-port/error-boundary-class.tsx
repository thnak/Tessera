// SPDX-License-Identifier: MIT

// Corpus (spike #660): class component error boundary

import { Component, ReactNode } from "react";

interface State {
  failed: boolean;
}

export class ErrorBoundary extends Component<{ children: ReactNode }, State> {
  state: State = { failed: false };
  static getDerivedStateFromError(): State {
    return { failed: true };
  }
  render() {
    return this.state.failed ? <p>Something went wrong</p> : this.props.children;
  }
}
