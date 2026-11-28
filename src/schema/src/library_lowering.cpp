// SPDX-License-Identifier: MIT

// Tags that lower onto library components (docs/specs/jsx-intrinsics-v1/html-aliases.md section
// 3.8, issue #1041): `input type="checkbox"|"radio"|"range"`, `progress`, `meter` and
// `details`. The alias table keeps the intrinsic carrier of each tag for the typings; this table
// is what the lowering reads to resolve the element to the imported component and to rewrite its
// attributes (`checked` -> `selected`, `onChange` -> `onToggle`, `aria-label` -> `label`).

#include "tables.hpp"

#include <span>
#include <string_view>

namespace tessera::schema::internal {
namespace {

template<typename Row, std::size_t N> constexpr std::span<const Row> rows(const Row (&list)[N]) {
    return std::span<const Row>(list, N);
}

constexpr std::string_view kFormRule = "form and validation belong to library `Form`/`Field`";

// Attributes every `input` control rejects the same way.
#define TESSERA_INPUT_COMMON_REJECTS                                                               \
    {"name", "", "", kFormRule}, {"required", "", "", kFormRule}, {"form", "", "", kFormRule},     \
        {"value", "", "", "the checked state is `checked`; a submitted value is a form rule"},     \
        {"autoFocus", "", "", "use `autoFocus` on a focusable `View`"}

constexpr LibraryAttr kCheckboxAttrs[] = {
    {"checked", "checked", "boolean", ""},
    {"disabled", "disabled", "boolean", ""},
    {"onChange", "onToggle", "() => void", ""},
    {"aria-label", "label", "string", ""},
    TESSERA_INPUT_COMMON_REJECTS,
    {"defaultChecked", "", "", "the library `Checkbox` is controlled: keep `checked` in state"},
};

constexpr LibraryAttr kRadioAttrs[] = {
    {"checked", "selected", "boolean", ""},
    {"disabled", "disabled", "boolean", ""},
    {"onChange", "onSelect", "() => void", ""},
    {"aria-label", "label", "string", ""},
    TESSERA_INPUT_COMMON_REJECTS,
    {"defaultChecked", "", "", "the library `Radio` is controlled: keep the selection in state"},
};

constexpr LibraryAttr kRangeAttrs[] = {
    {"defaultValue", "defaultValue", "number", ""},
    {"min", "min", "number", ""},
    {"max", "max", "number", ""},
    {"step", "step", "number", ""},
    {"aria-label", "label", "string", ""},
    TESSERA_INPUT_COMMON_REJECTS,
    {"onChange", "", "",
     "the library `Slider` keeps its value; a controlled range is tracked in #1051"},
    {"disabled", "", "", "the library `Slider` has no disabled state yet (#1051)"},
};

constexpr LibraryAttr kProgressAttrs[] = {
    {"value", "value", "number", ""},
    {"max", "max", "number", ""},
    {"aria-label", "label", "string", ""},
};

constexpr LibraryAttr kMeterAttrs[] = {
    {"value", "value", "number", ""},      {"min", "min", "number", ""},
    {"max", "max", "number", ""},          {"low", "low", "number", ""},
    {"high", "high", "number", ""},        {"optimum", "optimum", "number", ""},
    {"aria-label", "label", "string", ""},
};

constexpr LibraryAttr kDetailsAttrs[] = {
    {"open", "defaultOpen", "boolean", ""},
    {"name", "", "", "an exclusive group of disclosures is not in V1"},
    {"onToggle", "", "", "the open state is internal to the library `Disclosure`"},
};

#undef TESSERA_INPUT_COMMON_REJECTS

constexpr LibraryLowering kLowerings[] = {
    {"input", "checkbox", "Checkbox", "tessera-ui/Checkbox", rows(kCheckboxAttrs)},
    {"input", "radio", "Radio", "tessera-ui/Radio", rows(kRadioAttrs)},
    {"input", "range", "Slider", "tessera-ui/Slider", rows(kRangeAttrs)},
    {"progress", "", "Progress", "tessera-ui/Progress", rows(kProgressAttrs)},
    {"meter", "", "Meter", "tessera-ui/Meter", rows(kMeterAttrs)},
    {"details", "", "Disclosure", "tessera-ui/Disclosure", rows(kDetailsAttrs)},
};

} // namespace

std::span<const LibraryLowering> library_lowering_table() {
    return kLowerings;
}

} // namespace tessera::schema::internal
