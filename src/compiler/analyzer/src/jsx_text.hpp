// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::jsx {

struct EntityError {
    std::size_t offset = 0; // into the text passed to decode_entities
    std::size_t length = 0;
    std::string message;
};

// Decodes the entities of docs/specs/jsx-subset-v1.md section 10.4: `&amp; &lt; &gt; &quot;
// &apos; &nbsp;` and numeric `&#N;` / `&#xN;`. An `&name;` that is not one of them, or a numeric
// reference that is not a Unicode scalar value, is reported through `errors` (when not null) and
// left in the output unchanged. A `&` that does not start `&name;` is an ordinary character.
std::string decode_entities(std::string_view text, std::vector<EntityError>* errors);

// The standard JSX whitespace rule (section 10.4): lines are trimmed, blank lines dropped, the
// rest joined with one space; leading space is kept on the first line and trailing space on the
// last. Tabs count as spaces. Entities are decoded separately, after this step.
std::string collapse_jsx_text(std::string_view raw);

} // namespace tessera::compiler::jsx
