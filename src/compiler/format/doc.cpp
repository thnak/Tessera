// SPDX-License-Identifier: MIT

#include "doc.hpp"

#include <initializer_list>
#include <iterator>
#include <utility>

namespace tessera::compiler::format {
namespace {

enum class Mode : std::uint8_t { Flat, Break };

struct Cmd {
    int indent = 0;
    Mode mode = Mode::Break;
    DocId doc = 0;
    std::size_t offset = 0; // Fill only: index of the next content part
};

int display_width(std::string_view text) {
    int width = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++width;
        }
    }
    return width;
}

class Printer {
public:
    Printer(const DocArena& arena, int width, int indent_width)
        : arena_(arena),
          width_(width),
          indent_width_(indent_width) {}

    std::string run(DocId root) {
        stack_.push_back(Cmd{0, Mode::Break, root, 0});
        while (!stack_.empty()) {
            const Cmd cmd = stack_.back();
            stack_.pop_back();
            step(cmd);
        }
        flush_suffix();
        trim_line();
        while (!out_.empty() && out_.back() == '\n') {
            out_.pop_back();
        }
        out_.push_back('\n');
        return std::move(out_);
    }

private:
    void push(int indent, Mode mode, DocId doc, std::size_t offset = 0) {
        stack_.push_back(Cmd{indent, mode, doc, offset});
    }

    void trim_line() {
        while (!out_.empty() && out_.back() == ' ') {
            out_.pop_back();
        }
    }

    void flush_suffix() {
        for (const std::string& text : suffix_) {
            out_ += text;
            pos_ += display_width(text);
        }
        suffix_.clear();
    }

    void newline(int indent) {
        flush_suffix();
        trim_line();
        out_.push_back('\n');
        out_.append(static_cast<std::size_t>(indent), ' ');
        pos_ = indent;
    }

    // Whether `first`, printed in `mode`, and then the commands still on the stack fit in the
    // space left on the current line, up to the first newline.
    bool fits(std::initializer_list<Cmd> firsts, int remaining, bool rest_too) const {
        std::vector<Cmd> pending(std::rbegin(firsts), std::rend(firsts));
        std::size_t rest_index = stack_.size();
        while (remaining >= 0) {
            if (pending.empty()) {
                if (!rest_too || rest_index == 0) {
                    return true;
                }
                pending.push_back(stack_[--rest_index]);
            }
            const Cmd cmd = pending.back();
            pending.pop_back();
            const DocNode& node = arena_.node(cmd.doc);
            switch (node.kind) {
            case DocKind::Text:
                remaining -= display_width(node.text);
                break;
            case DocKind::Concat:
                for (auto it = node.kids.rbegin(); it != node.kids.rend(); ++it) {
                    pending.push_back(Cmd{cmd.indent, cmd.mode, *it, 0});
                }
                break;
            case DocKind::Indent:
                pending.push_back(Cmd{cmd.indent, cmd.mode, node.kids[0], 0});
                break;
            case DocKind::Group:
                pending.push_back(
                    Cmd{cmd.indent, node.forced ? Mode::Break : cmd.mode, node.kids[0], 0});
                break;
            case DocKind::Line:
                if (cmd.mode == Mode::Break) {
                    return true;
                }
                remaining -= 1;
                break;
            case DocKind::Softline:
                if (cmd.mode == Mode::Break) {
                    return true;
                }
                break;
            case DocKind::Hardline:
                return true;
            case DocKind::IfBreak:
                pending.push_back(
                    Cmd{cmd.indent, cmd.mode, node.kids[cmd.mode == Mode::Break ? 0 : 1], 0});
                break;
            case DocKind::Fill:
                for (auto it = node.kids.rbegin(); it != node.kids.rend(); ++it) {
                    pending.push_back(Cmd{cmd.indent, cmd.mode, *it, 0});
                }
                break;
            case DocKind::LineSuffix:
                break;
            }
        }
        return false;
    }

    void step(const Cmd& cmd) {
        const DocNode& node = arena_.node(cmd.doc);
        switch (node.kind) {
        case DocKind::Text:
            out_ += node.text;
            pos_ += display_width(node.text);
            break;
        case DocKind::Concat:
            for (auto it = node.kids.rbegin(); it != node.kids.rend(); ++it) {
                push(cmd.indent, cmd.mode, *it);
            }
            break;
        case DocKind::Indent:
            push(cmd.indent + indent_width_, cmd.mode, node.kids[0]);
            break;
        case DocKind::Group: {
            Mode mode = Mode::Break;
            if (!node.forced && cmd.mode == Mode::Flat) {
                mode = Mode::Flat;
            } else if (!node.forced) {
                mode = fits({Cmd{cmd.indent, Mode::Flat, node.kids[0], 0}}, width_ - pos_, true)
                           ? Mode::Flat
                           : Mode::Break;
            }
            push(cmd.indent, mode, node.kids[0]);
            break;
        }
        case DocKind::Line:
            if (cmd.mode == Mode::Flat) {
                out_.push_back(' ');
                ++pos_;
            } else {
                newline(cmd.indent);
            }
            break;
        case DocKind::Softline:
            if (cmd.mode == Mode::Break) {
                newline(cmd.indent);
            }
            break;
        case DocKind::Hardline:
            newline(cmd.indent);
            break;
        case DocKind::IfBreak:
            push(cmd.indent, cmd.mode, node.kids[cmd.mode == Mode::Break ? 0 : 1]);
            break;
        case DocKind::Fill:
            step_fill(cmd, node);
            break;
        case DocKind::LineSuffix:
            suffix_.push_back(node.text);
            break;
        }
    }

    // Prints one content part, then decides whether the separator after it stays on the line by
    // asking whether separator and the next content fit together.
    void step_fill(const Cmd& cmd, const DocNode& node) {
        const std::size_t index = cmd.offset;
        if (index >= node.kids.size()) {
            return;
        }
        const DocId content = node.kids[index];
        const bool content_fits =
            fits({Cmd{cmd.indent, Mode::Flat, content, 0}}, width_ - pos_, false);
        if (index + 1 >= node.kids.size()) {
            push(cmd.indent, content_fits ? Mode::Flat : Mode::Break, content);
            return;
        }
        const DocId separator = node.kids[index + 1];
        // Re-enter for the remaining parts after this pair is printed.
        push(cmd.indent, cmd.mode, cmd.doc, index + 2);
        if (index + 2 >= node.kids.size()) {
            push(cmd.indent, Mode::Flat, separator);
            push(cmd.indent, content_fits ? Mode::Flat : Mode::Break, content);
            return;
        }
        const DocId next = node.kids[index + 2];
        const bool pair_fits =
            fits({Cmd{cmd.indent, Mode::Flat, content, 0},
                  Cmd{cmd.indent, Mode::Flat, separator, 0}, Cmd{cmd.indent, Mode::Flat, next, 0}},
                 width_ - pos_, false);
        push(cmd.indent, pair_fits ? Mode::Flat : Mode::Break, separator);
        push(cmd.indent, content_fits ? Mode::Flat : Mode::Break, content);
    }

    const DocArena& arena_;
    int width_;
    int indent_width_;
    int pos_ = 0;
    std::string out_;
    std::vector<Cmd> stack_;
    std::vector<std::string> suffix_;
};

} // namespace

DocArena::DocArena() {
    empty_ = text("");
    line_ = add(DocNode{DocKind::Line, {}, {}, false});
    softline_ = add(DocNode{DocKind::Softline, {}, {}, false});
    hardline_ = add(DocNode{DocKind::Hardline, {}, {}, true});
}

DocId DocArena::add(DocNode node) {
    nodes_.push_back(std::move(node));
    return static_cast<DocId>(nodes_.size() - 1);
}

DocId DocArena::text(std::string value) {
    return add(DocNode{DocKind::Text, std::move(value), {}, false});
}

DocId DocArena::concat(std::vector<DocId> kids) {
    bool forced = false;
    for (const DocId kid : kids) {
        forced = forced || nodes_[kid].forced;
    }
    return add(DocNode{DocKind::Concat, {}, std::move(kids), forced});
}

DocId DocArena::indent(DocId inner) {
    return add(DocNode{DocKind::Indent, {}, {inner}, nodes_[inner].forced});
}

DocId DocArena::group(DocId inner) {
    return add(DocNode{DocKind::Group, {}, {inner}, nodes_[inner].forced});
}

DocId DocArena::if_break(DocId broken, DocId flat) {
    return add(DocNode{DocKind::IfBreak, {}, {broken, flat}, false});
}

DocId DocArena::fill(std::vector<DocId> parts) {
    bool forced = false;
    for (const DocId part : parts) {
        forced = forced || nodes_[part].forced;
    }
    return add(DocNode{DocKind::Fill, {}, std::move(parts), forced});
}

DocId DocArena::line_suffix(std::string value) {
    // A trailing line comment ends the line, so the group around it must not stay on one line.
    return add(DocNode{DocKind::LineSuffix, std::move(value), {}, true});
}

std::string print_doc(const DocArena& arena, DocId root, int width, int indent_width) {
    return Printer(arena, width, indent_width).run(root);
}

} // namespace tessera::compiler::format
