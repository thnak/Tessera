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
    // `TextInput`/`TextArea` stay `content-box` like Chrome's text fields (only buttons, selects
    // and checkboxes are border-box there), so an authored `height: 36px` plus a 1px border is a
    // 38px field in both (issue #973); the minimums are the border-box 176x22 / 161x38 less padding
    // and border.
    //
    // Controls: a browser's `button`/`input`/`textarea` do not inherit `line-height` (Chrome's
    // sheet sets `font: -webkit-small-control`, which resets it to `normal`), so a page with
    // `line-height: 1.5` on the body still has 20px buttons (found by the showcase parity check).
    //
    // `pre`, `blockquote`, `hr` (issue #541): Chrome's `pre` is a block with a 1em vertical margin
    // (the monospace face and `white-space: pre` are the alias's own defaults, not the sheet's);
    // `blockquote` has `margin: 1em 40px`; `hr` is a zero-height box whose 1px `inset` border is
    // drawn as a dark top and a light bottom edge, with a 0.5em vertical margin. They match through
    // the tag the alias keeps (`alias_tag`), because none of them has a role.
    //
    // `ul`/`ol` (issue #606): Chrome's `margin: 1em 0` and 40px start padding. The marker is the
    // inherited `list-style-type` (issue #541), Chrome's `disc`/`circle`/`square` by nesting depth
    // and `decimal` for `ol`; the painter draws it for a `role="listitem"` child, matched through
    // the tag the alias keeps like `pre` below.
    //
    // States (issue #606): Chrome fades a disabled control to 30% of its face, border and text (the
    // disabled rules come after hover/active so a disabled button does not react), and draws the
    // `:focus-visible` ring as a 2px #101010 outline. The engine has no `outline`, so the ring is a
    // zero-blur 2px `box-shadow` spread, the same pixels around the border box (outline-offset 0).
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
           "ul { list-style-type: disc; }\n"
           "ol { list-style-type: decimal; }\n"
           "ul ul, ol ul { list-style-type: circle; }\n"
           "ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square; }\n"
           "pre { display: block; margin: 1em 0; }\n"
           "blockquote { margin: 1em 40px; }\n"
           "hr { height: 0; margin: 0.5em 0; border-top-width: 1px; border-top-style: solid; "
           "border-top-color: #9a9a9a; border-bottom-width: 1px; border-bottom-style: solid; "
           "border-bottom-color: #eeeeee; }\n"
           "Button, TextInput, TextArea { line-height: normal; }\n"
           "Button { display: flex; flex-direction: row; align-items: center; "
           "justify-content: center; text-align: center; padding: 2px 7px; "
           "box-sizing: border-box; align-self: -tessera-inline; font-size: 13.3333px; color: "
           "#000000; "
           "background-color: #efefef; border: 1px solid #767676; border-radius: 2px; }\n"
           "Button:hover { background-color: #e5e5e5; border-color: #4f4f4f; }\n"
           "Button:active { background-color: #f5f5f5; border-color: #767676; }\n"
           "Button:disabled { color: rgba(16, 16, 16, 0.3); "
           "background-color: rgba(239, 239, 239, 0.3); border-color: rgba(118, 118, 118, 0.3); }\n"
           "Button:focus-visible, TextInput:focus-visible, TextArea:focus-visible "
           "{ box-shadow: 0 0 0 2px #101010; }\n"
           "TextInput:disabled, TextArea:disabled { color: #545454; "
           "background-color: rgba(239, 239, 239, 0.3); border-color: rgba(118, 118, 118, 0.3); }\n"
           "TextInput { min-width: 168px; "
           "min-height: 16px; padding: 2px 3px; font-size: 13.3333px; color: #000000; "
           "background-color: #ffffff; "
           "border: 1px solid #767676; border-radius: 2px; }\n"
           "TextArea { min-width: 153px; "
           "min-height: 32px; padding: 2px 3px; font-size: 13.3333px; color: #000000; "
           "background-color: #ffffff; "
           "border: 1px solid #767676; border-radius: 2px; }\n";
}

const PreparedStyleSheet& ua_prepared_stylesheet() {
    static const PreparedStyleSheet prepared = prepare_stylesheet(ua_stylesheet_text());
    return prepared;
}

} // namespace tessera::ui::style
