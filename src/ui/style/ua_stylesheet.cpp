// SPDX-License-Identifier: MIT

#include "tessera/ui/style/ua_stylesheet.hpp"

namespace tessera::ui::style {

std::string_view ua_stylesheet_text() noexcept {
    // `Button`: a browser's `<button>` centres its content on both axes, keeps `text-align: center`
    // and `padding: 1px 6px`, and is `box-sizing: border-box` (Chrome's UA sheet), so an authored
    // `width`/`height` stays the size of the box and the padding does not grow it. Without this a
    // bare Button drew its label at the box's top-left (issue #602). A flex row with both axes
    // centred is the same layout as the browser's anonymous inner box.
    return "Button { display: flex; flex-direction: row; align-items: center; "
           "justify-content: center; text-align: center; padding: 1px 6px; "
           "box-sizing: border-box; }\n";
}

const PreparedStyleSheet& ua_prepared_stylesheet() {
    static const PreparedStyleSheet prepared = prepare_stylesheet(ua_stylesheet_text());
    return prepared;
}

} // namespace tessera::ui::style
