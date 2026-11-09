// SPDX-License-Identifier: MIT

// Event payload records (docs/specs/jsx-intrinsics-v1/common-and-events.md section 2) and DOM
// element names (html-aliases.md section 4).

#include "tables.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::schema::internal {
namespace {

constexpr TypeField kPointerFields[] = {
    {"x", "float"},       {"y", "float"},
    {"pointerId", "int"}, {"pointerType", "\"mouse\" | \"touch\" | \"pen\""},
    {"button", "int"},    {"shift", "bool"},
    {"ctrl", "bool"},     {"alt", "bool"},
    {"meta", "bool"},
};

constexpr TypeField kClickFields[] = {
    {"x", "float"},
    {"y", "float"},
    {"pointerId", "int"},
    {"pointerType", "\"mouse\" | \"touch\" | \"pen\" | \"keyboard\" | \"assistive\""},
    {"button", "int"},
    {"shift", "bool"},
    {"ctrl", "bool"},
    {"alt", "bool"},
    {"meta", "bool"},
};

constexpr TypeField kKeyFields[] = {
    {"key", "string"}, {"code", "string"}, {"repeat", "bool"}, {"shift", "bool"},
    {"ctrl", "bool"},  {"alt", "bool"},    {"meta", "bool"},
};

constexpr TypeField kTextChangeFields[] = {
    {"value", "string"},
    {"target", "TextTarget"},
};

constexpr TypeField kTextSelectionFields[] = {
    {"anchor", "int"},
    {"focus", "int"},
};

constexpr TypeField kCompositionFields[] = {
    {"text", "string"},
    {"start", "int"},
    {"end", "int"},
};

constexpr TypeField kScrollFields[] = {
    {"offsetX", "float"},        {"offsetY", "float"},      {"viewportWidth", "float"},
    {"viewportHeight", "float"}, {"contentWidth", "float"}, {"contentHeight", "float"},
};

constexpr TypeField kErrorFields[] = {
    {"code", "string"},
    {"message", "string"},
};

constexpr TypeField kTextTargetFields[] = {
    {"value", "string"},
};

template<std::size_t N> constexpr std::span<const TypeField> fields(const TypeField (&list)[N]) {
    return std::span<const TypeField>(list, N);
}

constexpr std::array<TypeRecord, 11> kTypeRecords{{
    {"PointerEvent", fields(kPointerFields), "",
     "Node-local logical pixels; button 0 left, 1 middle, 2 right."},
    {"ClickEvent", fields(kClickFields), "",
     "Fields of PointerEvent; keyboard/assistive clicks use the node center and button 0."},
    {"KeyEvent", fields(kKeyFields), "", "W3C key value in `key`, physical code in `code`."},
    {"FocusEvent", {}, "", "No fields."},
    {"TextChangeEvent", fields(kTextChangeFields), "value",
     "`value` and `target.value` are equal; `value` is the Tessera form."},
    {"TextSubmitEvent", fields(kTextChangeFields), "value",
     "TextInput Enter; TextArea trigger [Q5]."},
    {"TextTarget", fields(kTextTargetFields), "",
     "The React-style `target.value` of a text event."},
    {"TextSelection", fields(kTextSelectionFields), "",
     "Grapheme offsets; caret affinity is not exposed."},
    {"CompositionEvent", fields(kCompositionFields), "", "IME composition updates."},
    {"ScrollEvent", fields(kScrollFields), "", "All sizes in dp."},
    {"ErrorInfo", fields(kErrorFields), "", "The safe subset of the runtime Error record."},
}};

constexpr std::array<DomType, 14> kDomTypes{{
    {"HTMLDivElement", "View"},
    {"HTMLElement", "View"},
    {"HTMLUListElement", "View"},
    {"HTMLOListElement", "View"},
    {"HTMLLIElement", "View"},
    {"HTMLButtonElement", "Button"},
    {"HTMLSpanElement", "Text"},
    {"HTMLParagraphElement", "Text"},
    {"HTMLHeadingElement", "Text"},
    {"HTMLLabelElement", "Text"},
    {"HTMLAnchorElement", "Text"},
    {"HTMLInputElement", "TextInput"},
    {"HTMLImageElement", "Image"},
    {"HTMLTextAreaElement", "TextArea"},
}};

template<typename Row> constexpr bool names_are_unique(const std::array<Row, 0>&) {
    return true;
}

template<typename Row, std::size_t N>
constexpr bool names_are_unique(const std::array<Row, N>& rows) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].name.empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < rows.size(); ++j) {
            if (rows[i].name == rows[j].name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(names_are_unique(kTypeRecords), "payload record names are unique");
static_assert(names_are_unique(kDomTypes), "DOM type names are unique");

} // namespace

std::span<const TypeRecord> type_record_table() {
    return kTypeRecords;
}

std::span<const DomType> dom_type_table() {
    return kDomTypes;
}

} // namespace tessera::schema::internal
