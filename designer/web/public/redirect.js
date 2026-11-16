// SPDX-License-Identifier: MIT

// The pre-merge URLs' redirect, shared by `scene.html` and `canvas.html` (issue #392).
//
// The merge replaced three pages with one, but links to the old two exist: in chat messages, in issues,
// in bookmarks, and -- the case that matters most -- in share links, whose `#p5=<payload>` fragment is
// the *whole* project state (docs/specs/designer-studio-v1.md section 7.1). A visitor who opens a
// `scene.html?...#p5=...` link made before the merge must land on the merged page with:
//
//   1. **the query intact** -- `location.search`, byte for byte, because the toolbar's own
//      `tessera.preview.p5` state and any future parameter a link carries must survive;
//   2. **the fragment intact** -- `location.hash`, byte for byte, because `#p5=` *is* the state, and a
//      re-encoded or truncated one would load different source than the one that was shared;
//   3. **the view selected** -- `?view=scene` / `?view=canvas`, so the pane the old page was is the
//      pane that opens. This is the one field the fragment cannot carry (section 7.1's envelope is
//      frozen: `v`, `s`, `w`, `h`, `d`, `t`, `f`), which is exactly why the old *path* has to be
//      translated here rather than guessed at on the merged page.
//
// **Why this runs before the page paints, and why it is a script and not a `<meta refresh>`.** A
// `refresh` cannot copy a fragment reliably and cannot add a query parameter, and it leaves the old
// URL in the history as a step the visitor can get stuck on. `location.replace` swaps the history
// entry for the new URL, so Back goes where the visitor came from rather than bouncing through the stub
// again. The script is inline and tiny (no import, no network, no engine code): a stub that had to
// fetch a module before redirecting would be a stub that can fail to redirect.
//
// **The hash is appended by hand, not passed to `URLSearchParams`.** A fragment is opaque: it may be a
// `#p5=` payload, a `#section-2` anchor, or a fragment whose base64url body is empty. Building the new
// URL through `new URL()` and then assigning `.hash` re-serializes it, and a payload that happens to end
// in `=` or contain a character the serializer treats specially would come back changed. String
// concatenation is the only version that is guaranteed byte-for-byte, so the function below does that
// and nothing else.
//
// `redirectTo(view)` is called by each stub with its own view id; it is a named function on
// `window` so a stub can be a two-line page and the logic stays in one readable place.

(function () {
  'use strict';

  /**
   * The merged page's URL for an old page's URL: same query (with `view` set), same fragment, and the
   * path swapped to `index.html`.
   *
   * `search` and `hash` are the old page's `location.search`/`.hash`, passed in rather than read here
   * so the function is testable without a browser: `scripts/ci/preview_page_checks.mjs` drives it with
   * the URL shapes a real share link takes.
   */
  function mergedUrl(view, search, hash) {
    // The query, with `view` set to this stub's view. Other parameters are preserved in their original
    // order and spelling: only the `view` parameter is ours to write.
    var query = String(search || '');
    if (query.charAt(0) === '?') {
      query = query.slice(1);
    }
    var kept = [];
    var replaced = false;
    if (query !== '') {
      var parts = query.split('&');
      for (var index = 0; index < parts.length; index += 1) {
        var part = parts[index];
        var equals = part.indexOf('=');
        var key = equals < 0 ? part : part.slice(0, equals);
        if (key === 'view') {
          if (!replaced) {
            kept.push('view=' + encodeURIComponent(view));
            replaced = true;
          }
          continue;
        }
        kept.push(part);
      }
    }
    if (!replaced) {
      kept.push('view=' + encodeURIComponent(view));
    }
    return 'index.html?' + kept.join('&') + String(hash || '');
  }

  /** Replaces the current history entry with the merged page, preserving query and fragment. */
  function redirectTo(view) {
    var location = globalThis.location;
    if (!location) {
      return;
    }
    var target = mergedUrl(view, location.search, location.hash);
    // `replace`, not `assign`: the stub must not become a history step. A visitor who presses Back
    // should leave the studio, not land on a page that immediately forwards them again.
    try {
      location.replace(target);
    } catch (error) {
      // A context that forbids navigation (some embedded webviews). The link below the fold is the
      // fallback the stub's markup carries, so the visitor is not stranded.
      var fallback = globalThis.document && globalThis.document.getElementById('continue');
      if (fallback) {
        fallback.href = target;
        fallback.hidden = false;
      }
    }
  }

  globalThis.tesseraRedirect = { mergedUrl: mergedUrl, redirectTo: redirectTo };
})();
