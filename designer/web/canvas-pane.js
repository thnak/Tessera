// SPDX-License-Identifier: MIT

// The canvas view's pane state (issue #392; the states are P3, issue #167). Split out of
// `view-canvas.js` so that file stays inside the repository's source-size limit, and because the four
// things a canvas pane can be showing are one subject:
//
//   unsupported  the browser has no `navigator.gpu`, or no adapter/device could be acquired, or the
//                module could not take the handles the page handed it. The pane *names the reason* and
//                points at the Scene tab, which needs no GPU. This is the default content of the pane,
//                because a blank canvas is the failure this slice exists to avoid.
//   failed       the module loaded and a frame could not be drawn; the message is the module's own.
//   frame        a device exists and the last frame produced counters worth showing.
//   (nothing)    the pane is idle before the first Run on this view.
//
// Keeping it here rather than inline is also what makes the draw path in `view-canvas.js` read as
// "acquire, draw, report" instead of being interrupted by eleven element lookups.

/** The pane's elements, resolved once per mount. */
export function canvasPane() {
  const pane = {
    status: document.getElementById('canvas-status'),
    unsupported: document.getElementById('unsupported'),
    unsupportedHeading: document.getElementById('unsupported-heading'),
    unsupportedBody: document.getElementById('unsupported-body'),
    frame: document.getElementById('canvas-frame'),
    canvas: document.getElementById('canvas'),
    failed: document.getElementById('failed'),
    failedBody: document.getElementById('failed-body'),
    frameStats: document.getElementById('frame-stats'),
    statFrame: document.getElementById('stat-frame'),
    statDraws: document.getElementById('stat-draws'),
    statVertices: document.getElementById('stat-vertices'),
    statNodes: document.getElementById('stat-nodes'),
    statDpr: document.getElementById('stat-dpr'),
  };

  function setStatus(text, kind) {
    pane.status.textContent = text;
    pane.status.className = 'status' + (kind ? ' ' + kind : '');
  }

  /**
   * Shows the named unsupported state. `reason` is a sentence from `canvas-device.js` (or the view),
   * never a bare code: the point of the state is that a designer learns something from it.
   */
  function showUnsupported(reason) {
    pane.unsupportedHeading.textContent = 'This browser cannot show the canvas preview';
    pane.unsupportedBody.textContent = reason;
    pane.unsupported.hidden = false;
    pane.frame.hidden = true;
    pane.frameStats.hidden = true;
    setStatus('WebGPU unavailable', 'warning');
  }

  function showFailed(message) {
    pane.failedBody.textContent = message;
    pane.failed.hidden = false;
    setStatus('failed', 'warning');
  }

  function hideStates() {
    pane.unsupported.hidden = true;
    pane.failed.hidden = true;
  }

  /**
   * Shows what a frame produced: the counters, and a named state when it produced nothing.
   *
   * A rejected frame (`hasDevice: false`) is the module refusing the handles the page handed it, which
   * is the unsupported state rather than a generic failure; a frame with `drawCalls: 0` is a real
   * answer that simply is not a picture, and saying so is the same choice the scene view makes about an
   * empty document.
   */
  function renderReport(report) {
    if (report.hasDevice !== true) {
      showUnsupported(report.error || 'The preview module got no WebGPU device.');
      return;
    }
    hideStates();
    pane.frame.hidden = false;
    pane.frameStats.hidden = false;
    pane.statFrame.textContent = report.width + 'x' + report.height;
    pane.statDraws.textContent = String(report.drawCalls);
    pane.statVertices.textContent = String(report.vertices);
    pane.statNodes.textContent = String(report.nodes);
    if (report.error) {
      showFailed(report.error);
    } else if (report.drawCalls === 0) {
      setStatus('no draw calls (the source produced no painted boxes)', 'warning');
    } else {
      setStatus('drew ' + report.drawCalls + ' draw call(s)', 'ok');
    }
  }

  return { ...pane, setStatus, showUnsupported, showFailed, hideStates, renderReport };
}
