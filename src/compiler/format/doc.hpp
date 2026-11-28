// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A small document algebra for the source formatter (ADR-061): text, line breaks that are either
// a space or a newline depending on whether the enclosing group fits, and the printer that decides.
// It is the Wadler/Prettier model reduced to what the TSX printer needs, so the layout rules stay
// in the builders and the width logic lives in one place.
namespace tessera::compiler::format {

using DocId = std::uint32_t;

enum class DocKind : std::uint8_t {
    Text,
    Concat,
    Indent,
    Group,
    Line,       // a space when the group is flat, a newline when it breaks
    Softline,   // nothing when flat, a newline when broken
    Hardline,   // always a newline; forces every enclosing group to break
    IfBreak,    // kids[0] when the group is broken, kids[1] when flat (either may be empty)
    Fill,       // alternating content and separator; breaks only the separators that must
    LineSuffix, // text held back until the end of the current line (trailing // comments)
};

struct DocNode {
    DocKind kind = DocKind::Text;
    std::string text;
    std::vector<DocId> kids;
    bool forced = false; // contains a Hardline or a line comment: a group around it must break
};

class DocArena {
public:
    DocArena();

    DocId text(std::string value);
    DocId concat(std::vector<DocId> kids);
    DocId indent(DocId inner);
    DocId group(DocId inner);
    DocId if_break(DocId broken, DocId flat);
    DocId fill(std::vector<DocId> parts);
    DocId line_suffix(std::string value);
    [[nodiscard]] DocId empty() const {
        return empty_;
    }
    [[nodiscard]] DocId line() const {
        return line_;
    }
    [[nodiscard]] DocId softline() const {
        return softline_;
    }
    [[nodiscard]] DocId hardline() const {
        return hardline_;
    }
    [[nodiscard]] bool forced(DocId id) const {
        return nodes_[id].forced;
    }
    [[nodiscard]] const DocNode& node(DocId id) const {
        return nodes_[id];
    }

private:
    DocId add(DocNode node);

    std::vector<DocNode> nodes_;
    DocId empty_ = 0;
    DocId line_ = 0;
    DocId softline_ = 0;
    DocId hardline_ = 0;
};

// Renders `root` with a line width of `width` columns and `indent_width` spaces per level. The
// result ends with exactly one newline and has no trailing spaces.
std::string print_doc(const DocArena& arena, DocId root, int width, int indent_width);

} // namespace tessera::compiler::format
