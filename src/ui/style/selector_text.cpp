// SPDX-License-Identifier: MIT

#include "selector_text.hpp"

#include <cstddef>
#include <string_view>

namespace tessera::ui::style {
namespace {

void append_compound(std::string& out, const CompoundSelector& compound);

// One attribute selector: `[name]`, `[name=value]`, `[name~="value" i]`. The value is quoted
// whenever it was quoted in the source or holds a character that would not re-parse bare, so the
// printed form is always a selector the parser accepts again.
void append_attribute(std::string& out, const AttributeSelector& attribute) {
    out.push_back('[');
    out += attribute.name;
    const char* op = nullptr;
    switch (attribute.op) {
    case AttributeOperator::Exists:
        break;
    case AttributeOperator::Equals:
        op = "=";
        break;
    case AttributeOperator::Includes:
        op = "~=";
        break;
    case AttributeOperator::Dash:
        op = "|=";
        break;
    case AttributeOperator::Prefix:
        op = "^=";
        break;
    case AttributeOperator::Suffix:
        op = "$=";
        break;
    case AttributeOperator::Substring:
        op = "*=";
        break;
    }
    if (op != nullptr) {
        out += op;
        out.push_back('"');
        out += attribute.value;
        out.push_back('"');
        if (attribute.case_insensitive) {
            out += " i";
        }
    }
    out.push_back(']');
}

void append_pseudo(std::string& out, const PseudoClass& pseudo) {
    out += pseudo.pseudo_element ? "::" : ":";
    out += pseudo.name;
    if (pseudo.nested != nullptr) {
        out.push_back('(');
        out += selector_list_text(*pseudo.nested);
        out.push_back(')');
        return;
    }
    if (!pseudo.argument.empty()) {
        out.push_back('(');
        out += pseudo.argument;
        out.push_back(')');
    }
}

void append_compound(std::string& out, const CompoundSelector& compound) {
    std::string text;
    if (!compound.tag.empty()) {
        text += compound.tag;
    }
    if (!compound.id.empty()) {
        text.push_back('#');
        text += compound.id;
    }
    for (const std::string& name : compound.classes) {
        text.push_back('.');
        text += name;
    }
    for (const AttributeSelector& attribute : compound.attributes) {
        append_attribute(text, attribute);
    }
    for (const PseudoClass& pseudo : compound.pseudos) {
        append_pseudo(text, pseudo);
    }
    // A compound with no simple selector at all is the universal selector, which the parser writes
    // as an empty tag. Printing nothing would make `* > .a` read as `> .a`, so it is spelled out.
    out += text.empty() ? std::string_view("*") : std::string_view(text);
}

const char* combinator_text(Combinator combinator) {
    switch (combinator) {
    case Combinator::Descendant:
        return " ";
    case Combinator::Child:
        return " > ";
    case Combinator::AdjacentSibling:
        return " + ";
    case Combinator::GeneralSibling:
        return " ~ ";
    }
    return " ";
}

} // namespace

std::string selector_list_text(const SelectorList& selectors) {
    std::string out;
    for (std::size_t index = 0; index < selectors.selectors.size(); ++index) {
        if (index != 0) {
            out += ", ";
        }
        const ComplexSelector& complex = selectors.selectors[index];
        for (std::size_t part = 0; part < complex.compounds.size(); ++part) {
            if (part != 0) {
                const std::size_t combinator = part - 1;
                out += combinator < complex.combinators.size()
                           ? combinator_text(complex.combinators[combinator])
                           : " ";
            }
            append_compound(out, complex.compounds[part]);
        }
    }
    return out;
}

} // namespace tessera::ui::style
