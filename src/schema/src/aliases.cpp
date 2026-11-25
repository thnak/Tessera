// SPDX-License-Identifier: MIT

// Lowercase HTML tag aliases (docs/specs/jsx-intrinsics-v1/html-aliases.md): the tag table of
// section 2, the attribute rewrites of section 3 and the unmapped tags of section 5. Section 5
// groups are expanded here to one row per tag so consumers need no group lookup.

#include "tables.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::schema::internal {
namespace {

template<typename Row, std::size_t N> constexpr std::span<const Row> rows(const Row (&list)[N]) {
    return std::span<const Row>(list, N);
}

// --- injected props -------------------------------------------------------------------------

constexpr AliasInject kInjectRoleList[] = {{"role", "list"}};
constexpr AliasInject kInjectRoleListItem[] = {{"role", "listitem"}};
// `h1`-`h6` also carry their level as `aria-level`, which the user-agent sheet sizes by.
constexpr std::string_view kHeadingNote =
    "the level is `aria-level`; the user-agent sheet sizes it";
constexpr AliasInject kInjectHeading[6][2] = {
    {{"role", "heading"}, {"aria-level", "1"}}, {{"role", "heading"}, {"aria-level", "2"}},
    {{"role", "heading"}, {"aria-level", "3"}}, {{"role", "heading"}, {"aria-level", "4"}},
    {{"role", "heading"}, {"aria-level", "5"}}, {{"role", "heading"}, {"aria-level", "6"}}};
constexpr AliasInject kInjectRoleParagraph[] = {{"role", "paragraph"}};
constexpr AliasInject kInjectRoleLink[] = {{"role", "link"}};

// `input type="password"` injects secure; the text-ish types inject inputMode.
constexpr AliasInject kInjectSecure[] = {{"secure", "true"}};
constexpr AliasInject kInjectModeEmail[] = {{"inputMode", "email"}};
constexpr AliasInject kInjectModeTel[] = {{"inputMode", "tel"}};
constexpr AliasInject kInjectModeUrl[] = {{"inputMode", "url"}};
constexpr AliasInject kInjectModeSearch[] = {{"inputMode", "search"}};
constexpr AliasInject kInjectModeNumeric[] = {{"inputMode", "numeric"}};

constexpr AliasTypeMap kInputTypeMap[] = {
    {"password", rows(kInjectSecure)},   {"email", rows(kInjectModeEmail)},
    {"tel", rows(kInjectModeTel)},       {"url", rows(kInjectModeUrl)},
    {"search", rows(kInjectModeSearch)}, {"number", rows(kInjectModeNumeric)},
};

// --- attribute rewrites ---------------------------------------------------------------------

constexpr AliasValue kButtonTypeValues[] = {
    {"button", "", false, ""},
    {"submit", "", true, "V1 has no form element; use `onClick`"},
    {"reset", "", true, "V1 has no form element; use `onClick`"},
};

constexpr AliasAttr kButtonAttrs[] = {
    {"type", AliasAttrKind::ValueMap, "", "V1 has no form element; use `onClick`",
     rows(kButtonTypeValues), "must be a string literal"},
};

constexpr AliasValue kWrapValues[] = {
    {"soft", "word", false, ""},
    {"off", "none", false, ""},
    {"hard", "", true, "HTML hard wrapping has no V1 equivalent"},
};

constexpr AliasAttr kTextAreaAttrs[] = {
    {"wrap", AliasAttrKind::ValueMap, "", "HTML hard wrapping has no V1 equivalent",
     rows(kWrapValues), ""},
    {"cols", AliasAttrKind::Reject, "", "set a width in `style`", {}, ""},
};

constexpr AliasAttr kInputAttrs[] = {
    {"autoComplete",
     AliasAttrKind::Rename,
     "autofill",
     "",
     {},
     "tokens email/username/current-password/new-password/one-time-code/name/tel/"
     "street-address/postal-code pass; `off` is dropped; any other token is HTML_ATTR [Q19]"},
    {"checked",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"defaultChecked",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"name",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"required",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"pattern",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"min",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"max",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"step",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"minLength",
     AliasAttrKind::Reject,
     "",
     "use `maxLength`; minimum length is a form rule",
     {},
     ""},
    {"multiple", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"accept", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"list", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"form", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"size", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"autoFocus",
     AliasAttrKind::Reject,
     "",
     "not in V1 on inputs; use it on a focusable `View`",
     {},
     ""},
    {"spellCheck", AliasAttrKind::Reject, "", "not in V1", {}, ""},
    {"autoCapitalize", AliasAttrKind::Reject, "", "not in V1", {}, ""},
    {"autoCorrect", AliasAttrKind::Reject, "", "not in V1", {}, ""},
};

constexpr AliasAttr kImgAttrs[] = {
    {"width",
     AliasAttrKind::ToStyle,
     "width",
     "",
     {},
     "a number or numeric string is dp; with `style.width` it is TESSERA_JSX_PROP_CONFLICT"},
    {"height",
     AliasAttrKind::ToStyle,
     "height",
     "",
     {},
     "a number or numeric string is dp; with `style.height` it is TESSERA_JSX_PROP_CONFLICT"},
    {"srcSet", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"sizes", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"loading", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"decoding", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"crossOrigin", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"onLoad", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"onError", AliasAttrKind::Reject, "", "no V1 meaning; use ErrorBoundary", {}, ""},
};

constexpr AliasAttr kAnchorAttrs[] = {
    {"href", AliasAttrKind::Reject, "", "no `href` in V1; use `onClick`", {}, ""},
    {"target", AliasAttrKind::Reject, "", "no `href` in V1; use `onClick`", {}, ""},
    {"rel", AliasAttrKind::Reject, "", "no `href` in V1; use `onClick`", {}, ""},
    {"download", AliasAttrKind::Reject, "", "no `href` in V1; use `onClick`", {}, ""},
};

// --- tag table ------------------------------------------------------------------------------

// Injected default run styles of the emphasis and group A aliases (html-aliases.md section 2,
// elements-basic.md section 2.1). Grouped by tag: `alias_style_defaults` returns the contiguous
// group. These are defaults, not wrapper styles: an explicit className/style of the same property
// wins in the cascade.
constexpr AliasStyle kAliasStyles[] = {
    {"b", "font-weight", "700"},
    {"cite", "font-style", "italic"},
    {"code", "font-family", "monospace"},
    {"em", "font-style", "italic"},
    {"i", "font-style", "italic"},
    {"kbd", "font-family", "monospace"},
    {"mark", "background-color", "#ffff00"},
    {"mark", "color", "#000000"},
    {"pre", "font-family", "monospace"},
    {"pre", "white-space", "pre"},
    {"s", "text-decoration-line", "line-through"},
    {"small", "font-size", "0.83em"},
    {"strong", "font-weight", "700"},
    {"sub", "font-size", "0.83em"},
    {"sub", "vertical-align", "sub"},
    {"sup", "font-size", "0.83em"},
    {"sup", "vertical-align", "super"},
    {"u", "text-decoration-line", "underline"},
};

// Section 5 alternatives, one text per group.
constexpr std::string_view kAltForm =
    "No `form` in V1: `TextInput` `onSubmit` plus `Button` `onClick`; library `Form`/`Field` "
    "after V1";
constexpr std::string_view kAltSelect = "Library `Select` (after V1)";
constexpr std::string_view kAltTable =
    "`View` with flex or grid style; `VirtualGrid` or library `DataGrid` for large data";
constexpr std::string_view kAltSvg = "`Path` with `d` and `viewBox`";
constexpr std::string_view kAltMedia = "`CustomElement`";
constexpr std::string_view kAltOverlay = "Overlay service and library components";
constexpr std::string_view kAltProgress = "Library `Progress` (after V1)";
constexpr std::string_view kAltDocument =
    "None: the app shell, `tessera.json` and `import \"./file.css\"` replace them";

// A row with no injected props, attribute rewrites or type map.
constexpr AliasRow plain(std::string_view tag, std::string_view target, AliasStatus status,
                         std::string_view note) {
    return AliasRow{tag, target, status, {}, {}, {}, false, "", note};
}

constexpr AliasRow kAliases[] = {
    // Section 2: mapped and partial.
    {"div", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    {"section", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    {"article", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    {"main", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    {"header", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    {"footer", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    plain("nav", "View", AliasStatus::Mapped,
          "landmark roles are not in the accessibility role list, so none is injected"),
    plain("aside", "View", AliasStatus::Mapped,
          "landmark roles are not in the accessibility role list, so none is injected"),
    {"figure", "View", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    {"ul",
     "View",
     AliasStatus::Mapped,
     rows(kInjectRoleList),
     {},
     {},
     false,
     "",
     "no list markers and no numbering"},
    {"ol",
     "View",
     AliasStatus::Mapped,
     rows(kInjectRoleList),
     {},
     {},
     false,
     "",
     "no list markers and no numbering"},
    {"li", "View", AliasStatus::Mapped, rows(kInjectRoleListItem), {}, {}, false, "", ""},
    {"span", "Text", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    plain("b", "Text", AliasStatus::Mapped, "inline span; injects font-weight: 700"),
    plain("strong", "Text", AliasStatus::Mapped, "inline span; injects font-weight: 700"),
    plain("i", "Text", AliasStatus::Mapped, "inline span; injects font-style: italic"),
    plain("em", "Text", AliasStatus::Mapped, "inline span; injects font-style: italic"),
    plain(
        "code", "Text", AliasStatus::Mapped,
        "inline span; injects font-family: monospace (no font selection yet, so no metric change)"),
    {"p", "Text", AliasStatus::Mapped, rows(kInjectRoleParagraph), {}, {}, false, "", ""},
    plain("label", "Text", AliasStatus::Mapped,
          "does not focus its control; `htmlFor` is rejected [Q11]"),
    {"figcaption", "Text", AliasStatus::Mapped, {}, {}, {}, false, "", ""},
    // clang-format off
    {"h1", "Text", AliasStatus::Mapped, rows(kInjectHeading[0]), {}, {}, false, "", kHeadingNote},
    {"h2", "Text", AliasStatus::Mapped, rows(kInjectHeading[1]), {}, {}, false, "", kHeadingNote},
    {"h3", "Text", AliasStatus::Mapped, rows(kInjectHeading[2]), {}, {}, false, "", kHeadingNote},
    {"h4", "Text", AliasStatus::Mapped, rows(kInjectHeading[3]), {}, {}, false, "", kHeadingNote},
    {"h5", "Text", AliasStatus::Mapped, rows(kInjectHeading[4]), {}, {}, false, "", kHeadingNote},
    {"h6", "Text", AliasStatus::Mapped, rows(kInjectHeading[5]), {}, {}, false, "", kHeadingNote},
    // clang-format on
    {"a",
     "Text",
     AliasStatus::Partial,
     rows(kInjectRoleLink),
     rows(kAnchorAttrs),
     {},
     false,
     "",
     "`href`, `target`, `rel` and `download` are rejected; use `onClick` [Q11]"},
    {"button",
     "Button",
     AliasStatus::Mapped,
     {},
     rows(kButtonAttrs),
     {},
     false,
     "",
     "`type=\"button\"` is accepted and dropped; submit/reset are rejected"},
    {"input",
     "TextInput",
     AliasStatus::Mapped,
     {},
     rows(kInputAttrs),
     rows(kInputTypeMap),
     true,
     "",
     "`type` must be a string literal and is removed; [Q19] `autoComplete` tokens"},
    {"textarea", "TextArea", AliasStatus::Mapped, {}, rows(kTextAreaAttrs), {}, false, "", ""},
    {"img", "Image", AliasStatus::Mapped, {}, rows(kImgAttrs), {}, false, "", ""},
    plain("u", "Text", AliasStatus::Mapped, "inline span; injects text-decoration-line: underline"),
    plain("s", "Text", AliasStatus::Mapped,
          "inline span; injects text-decoration-line: line-through"),
    plain("small", "Text", AliasStatus::Partial,
          "inline span; the size change is dropped inside a `Text` (a span keeps the parent's "
          "metrics); injects font-size: 0.83em"),
    plain("mark", "Text", AliasStatus::Mapped,
          "inline span; injects a yellow background and black text"),
    plain(
        "kbd", "Text", AliasStatus::Mapped,
        "inline span; injects font-family: monospace (no font selection yet, so no metric change)"),
    plain("sub", "Text", AliasStatus::Partial,
          "inline span; the size change is dropped inside a `Text` (a span keeps the parent's "
          "metrics); injects vertical-align: sub and font-size: 0.83em"),
    plain("sup", "Text", AliasStatus::Partial,
          "inline span; the size change is dropped inside a `Text` (a span keeps the parent's "
          "metrics); injects vertical-align: super and font-size: 0.83em"),
    plain("abbr", "Text", AliasStatus::Mapped,
          "inline span; `title` is rejected, so no dotted underline"),
    plain("cite", "Text", AliasStatus::Mapped, "inline span; injects font-style: italic"),
    plain("q", "Text", AliasStatus::Mapped,
          "inline span; lowering wraps the content in the curly quotes a browser draws"),
    plain("br", "Text", AliasStatus::Mapped,
          "void; inside a `Text` it is a forced line break, as a node of its own it is an empty "
          "`Text`"),
    plain("hr", "View", AliasStatus::Mapped, "void; the user-agent sheet draws the 2px inset rule"),
    plain("pre", "Text", AliasStatus::Mapped,
          "injects white-space: pre and font-family: monospace; the user-agent sheet makes it a "
          "block"),
    plain("blockquote", "View", AliasStatus::Mapped,
          "the user-agent sheet gives it the 1em 40px margin"),

    // Section 5: no mapping.
    {"form", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltForm, ""},
    {"select", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSelect, ""},
    {"option", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSelect, ""},
    {"optgroup", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSelect, ""},
    {"datalist", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSelect, ""},
    {"table", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"thead", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"tbody", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"tfoot", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"tr", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"td", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"th", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"caption", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"colgroup", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"col", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltTable, ""},
    {"svg", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"g", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"circle", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"rect", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"line", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"polyline", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"polygon", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"ellipse", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"text", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"defs", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"use", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"path", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltSvg, ""},
    {"canvas", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"video", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"audio", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"iframe", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"embed", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"object", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"picture", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"source", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"track", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltMedia, ""},
    {"dialog", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltOverlay, ""},
    {"details", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltOverlay, ""},
    {"summary", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltOverlay, ""},
    {"menu", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltOverlay, ""},
    {"popover", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltOverlay, ""},
    {"progress", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltProgress, ""},
    {"meter", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltProgress, ""},
    {"html", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"head", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"body", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"title", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"meta", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"link", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"script", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"style", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"base", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"noscript", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"template", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
    {"slot", "", AliasStatus::Unsupported, {}, {}, {}, false, kAltDocument, ""},
};

constexpr bool alias_rows_are_valid(std::span<const AliasRow> list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].tag.empty()) {
            return false;
        }
        if (list[i].status == AliasStatus::Unsupported) {
            if (!list[i].target.empty() || list[i].alternative.empty()) {
                return false;
            }
        } else if (list[i].target.empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].tag == list[j].tag) {
                return false;
            }
        }
    }
    return true;
}

static_assert(alias_rows_are_valid(kAliases), "alias rows are unique and well-formed");
// html-aliases.md section 2 (46 mapped or partial tags) and section 5 (55 unmapped tags).
static_assert(std::size(kAliases) == 101, "46 mapped or partial tags plus 55 unmapped tags");

constexpr bool alias_styles_are_grouped(std::span<const AliasStyle> list) {
    for (std::size_t i = 1; i < list.size(); ++i) {
        if (list[i].tag < list[i - 1].tag) {
            return false;
        }
    }
    return true;
}

static_assert(alias_styles_are_grouped(std::span<const AliasStyle>(kAliasStyles)),
              "alias style defaults are grouped by tag for the subspan lookup");
static_assert(std::size(kAliasStyles) == 18, "emphasis and group A default styles");

} // namespace

std::span<const AliasRow> alias_table() {
    return kAliases;
}

std::span<const AliasStyle> alias_style_table() {
    return std::span<const AliasStyle>(kAliasStyles);
}

} // namespace tessera::schema::internal
