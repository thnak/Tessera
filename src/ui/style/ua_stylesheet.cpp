// SPDX-License-Identifier: MIT

#include "tessera/ui/style/ua_stylesheet.hpp"

namespace tessera::ui::style {

std::string_view ua_stylesheet_text() noexcept {
    // `Button`: a browser's `<button>` centres its content on both axes, keeps `text-align: center`
    // and `padding: 1px 6px` inside a 2px border (laid out as a 1px painted border and
    // `padding: 2px 7px`, the same box), and is `box-sizing: border-box` (Chrome's UA sheet), so
    // an authored `width`/`height` stays the size of the box and the padding does not grow it.
    // Without this a bare Button drew its label at the box's top-left (issue #602). A flex row with
    // both axes centred is the same layout as the browser's anonymous inner box.
    //
    // `View`: a browser's `div`/`section`/... is `display: block`, children stacked and stretched
    // to the width. The engine's own default for a box is a flex *row*, which put the children of
    // an unstyled `div` side by side (issue #606 audit). `block` is the adapter's column flex
    // container with stretch, the same placement.
    //
    // `h1`-`h6` (issue #606): bold, and the size and margins of Chrome's UA sheet per level
    // (`2em`/`0.67em` for h1 down to `0.67em`/`2.33em` for h6), written as Chrome writes them: `em`
    // is the element's own font size, which the cascade resolves first (issue #608). The level is
    // the `aria-level` the alias injects, which the style tree exposes
    // as an attribute. A browser collapses the margin
    // between two stacked blocks; a flex column does not, so two adjacent headings sit further
    // apart than in a browser (a named exception, docs/reference/ua-audit.md).
    //
    // Controls: a browser's `button`/`input`/`textarea` do not inherit `line-height` (Chrome's
    // sheet sets `font: -webkit-small-control`, which resets it to `normal`), so a page with
    // `line-height: 1.5` on the body still has 20px buttons (found by the showcase parity check).
    //
    // `ul`/`ol` (issue #606): Chrome's `margin: 1em 0` and 40px start padding. The marker itself
    // is not drawn (a named exception).
    return "View { display: block; }\n"
           "Text[role=\"heading\"] { display: block; font-weight: 700; }\n"
           "Text[aria-level=\"1\"] { font-size: 2em; margin: 0.67em 0; }\n"
           "Text[aria-level=\"2\"] { font-size: 1.5em; margin: 0.83em 0; }\n"
           "Text[aria-level=\"3\"] { font-size: 1.17em; margin: 1em 0; }\n"
           "Text[aria-level=\"4\"] { font-size: 1em; margin: 1.33em 0; }\n"
           "Text[aria-level=\"5\"] { font-size: 0.83em; margin: 1.67em 0; }\n"
           "Text[aria-level=\"6\"] { font-size: 0.67em; margin: 2.33em 0; }\n"
           "Text[role=\"paragraph\"] { display: block; margin: 1em 0; }\n"
           "Text[role=\"link\"] { color: #0000ee; text-decoration: underline; }\n"
           "View[role=\"list\"] { margin: 1em 0; padding-left: 40px; }\n"
           "Button, TextInput, TextArea { line-height: normal; }\n"
           "Button { display: flex; flex-direction: row; align-items: center; "
           "justify-content: center; text-align: center; padding: 2px 7px; "
           "box-sizing: border-box; align-self: flex-start; font-size: 13.3333px; color: #000000; "
           "background-color: #efefef; border: 1px solid #767676; border-radius: 2px; }\n"
           "Button:hover { background-color: #e5e5e5; border-color: #4f4f4f; }\n"
           "Button:active { background-color: #f5f5f5; border-color: #767676; }\n"
           "TextInput { box-sizing: border-box; min-width: 176px; "
           "min-height: 22px; padding: 2px 3px; font-size: 13.3333px; color: #000000; "
           "background-color: #ffffff; "
           "border: 1px solid #767676; border-radius: 2px; }\n"
           "TextArea { box-sizing: border-box; min-width: 161px; "
           "min-height: 38px; padding: 2px 3px; font-size: 13.3333px; color: #000000; "
           "background-color: #ffffff; "
           "border: 1px solid #767676; border-radius: 2px; }\n";
}

const PreparedStyleSheet& ua_prepared_stylesheet() {
    static const PreparedStyleSheet prepared = prepare_stylesheet(ua_stylesheet_text());
    return prepared;
}

} // namespace tessera::ui::style
