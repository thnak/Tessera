// SPDX-License-Identifier: MIT

// `source.patch` (issue #171, designer-protocol-v1 section 10.2): the commit's edit list.
//
// **What these tests are for.** A patch is the one thing in this slice that can corrupt a
// developer's file, so every assertion here is about a byte range: that it covers exactly the text
// it claims to, that applying the list in the given order gives the same file as splicing every
// range against the original offsets at once, and that a range never lands inside a UTF-8 code
// point. The tests slice the source text they were given and compare it with what the edit says it
// replaces, rather than comparing against a hard-coded offset - an off-by-one that a constant would
// happily agree with is exactly the failure this file exists to catch.

#include "scene_canvas_patch_support.hpp"

#include <cstdint>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace patch_test;

// The failure counter and the assertion, in the shape the sibling canvas tests use
// (`scene_canvas_style_tests.cpp`): one counter, one message per failure, `main()` reports them
// once. The prefix names this file so a mixed run says which suite failed.
int& failures() {
    static int count = 0;
    return count;
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_canvas_patch: " << message << "\n";
        ++failures();
    }
}

void test_inline_replaces_the_style_attribute() {
    PatchContext context = context_for(kInlineSource);
    point_at_view(context, kInlineSource);
    const auto patch = cli::build_source_patch(inline_request(), context);
    expect(patch.has_value(), "an inline commit produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 1, "an inline commit is one edit");
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    expect(edit.file == "ui/Card.tsx", "the edit names the component file");
    // The range must cover EXACTLY the `style` attribute: the text it replaces is the attribute,
    // and the text around it is untouched. This is the assertion a constant offset would not catch.
    const std::string_view replaced = kInlineSource.substr(edit.offset, edit.length);
    expect(replaced == R"(style={{ color: "red" }})",
           "the edit replaces exactly the style attribute, got `" + std::string(replaced) + "`");
    expect(edit.new_text == R"(style={{ color: "red", paddingLeft: "8px" }})",
           "the new text keeps the existing declaration and adds the commit's, got `" +
               edit.new_text + "`");
    // The attribute before the edit and the element after it are byte-identical.
    expect(kInlineSource.substr(0, edit.offset).ends_with("className=\"card\" "),
           "the text before the attribute is untouched");
    expect(kInlineSource.substr(edit.offset + edit.length).starts_with(">"),
           "the text after the attribute is untouched");
}

void test_inline_inserts_a_missing_style_attribute() {
    PatchContext context = context_for(kNoStyleSource);
    point_at_view(context, kNoStyleSource);
    const auto patch = cli::build_source_patch(inline_request(), context);
    expect(patch.has_value(), "an inline commit with no style attribute produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 1, "the insert is one edit");
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    // An insert, not a replace: `length == 0` is what tells a host to insert rather than overwrite.
    expect(edit.length == 0, "creating a missing attribute inserts (`length == 0`)");
    expect(edit.new_text == R"( style={{ paddingLeft: "8px" }})",
           "the inserted text is the whole attribute, got `" + edit.new_text + "`");
    const std::string applied = apply_in_order(std::string(kNoStyleSource), patch->edits);
    expect(applied.find(R"(<View className="card" style={{ paddingLeft: "8px" }}>)") !=
               std::string::npos,
           "the applied text has the attribute inside the opening tag");
    // The element still parses: the insert did not land inside the tag name or after the `>`.
    expect(applied.find("<View className=\"card\" style=") != std::string::npos,
           "the attribute is inserted after the existing one");
}

void test_inline_range_is_exact_on_a_unicode_source() {
    PatchContext context = context_for(kUnicodeSource);
    point_at_view(context, kUnicodeSource);
    const auto patch = cli::build_source_patch(inline_request(), context);
    expect(patch.has_value(), "a commit on a source with non-ASCII text produces a patch");
    if (!patch) {
        return;
    }
    for (const TextEdit& edit : patch->edits) {
        expect(on_boundary(kUnicodeSource, edit.offset),
               "the edit's offset is on a UTF-8 code point boundary");
        expect(on_boundary(kUnicodeSource, edit.offset + edit.length),
               "the edit's end is on a UTF-8 code point boundary");
    }
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    expect(kUnicodeSource.substr(edit.offset, edit.length) == R"(style={{ color: "red" }})",
           "the range is exact even with a two-byte character earlier in the file");
    const std::string applied = apply_in_order(std::string(kUnicodeSource), patch->edits);
    expect(applied.find("café") != std::string::npos,
           "the non-ASCII text before the edit is unchanged");
}

void test_class_appends_a_missing_rule() {
    PatchContext context = context_for(kNoStyleSource, kEmptyStylesheet);
    point_at_view(context, kNoStyleSource);
    PatchRequest request;
    request.destination = PatchDestination::Class;
    request.class_name = "card";
    request.file = "ui/Card.css";
    request.declarations = {{"padding-left", "8px"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(patch.has_value(), "a class commit with no rule produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 1, "appending a rule is one edit");
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    expect(edit.file == "ui/Card.css", "the edit names the CSS file");
    expect(edit.offset == kEmptyStylesheet.size(), "the rule is appended at the end of the file");
    expect(edit.length == 0, "appending a rule inserts");
    expect(edit.new_text == ".card {\n  padding-left: 8px;\n}\n",
           "the appended rule is the class and its declaration, got `" + edit.new_text + "`");
    const std::string applied = apply_in_order(std::string(kEmptyStylesheet), patch->edits);
    expect(applied.find(".other") != std::string::npos, "the existing rule is untouched");
    expect(applied.find(".card {\n  padding-left: 8px;\n}") != std::string::npos,
           "the appended rule is in the applied text");
}

void test_class_edits_an_existing_rule() {
    PatchContext context = context_for(kNoStyleSource, kStylesheet);
    point_at_view(context, kNoStyleSource);
    PatchRequest request;
    request.destination = PatchDestination::Class;
    request.class_name = "card";
    request.file = "ui/Card.css";
    request.declarations = {{"color", "red"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(patch.has_value(), "a class commit on an existing rule produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 1, "editing one declaration is one edit");
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    const std::string_view replaced = kStylesheet.substr(edit.offset, edit.length);
    expect(replaced == "color: blue;",
           "the edit replaces exactly the declaration, got `" + std::string(replaced) + "`");
    expect(edit.new_text == "color: red;", "the new declaration is the commit's value");
    const std::string applied = apply_in_order(std::string(kStylesheet), patch->edits);
    expect(applied.find(".other {\n  color: green;\n}") != std::string::npos,
           "the other rule is untouched");
    expect(applied.find(".card {\n  color: red;\n}") != std::string::npos,
           "the edited rule carries the new value");
}

void test_class_inserts_a_declaration_the_rule_lacks() {
    PatchContext context = context_for(kNoStyleSource, kStylesheet);
    point_at_view(context, kNoStyleSource);
    PatchRequest request;
    request.destination = PatchDestination::Class;
    request.class_name = "card";
    request.file = "ui/Card.css";
    request.declarations = {{"padding-left", "8px"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(patch.has_value(), "a class commit adding a property produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 1, "adding one declaration is one edit");
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    expect(edit.length == 0, "a declaration the rule lacks is inserted");
    const std::string applied = apply_in_order(std::string(kStylesheet), patch->edits);
    expect(applied.find(".card {\n  color: blue;\n  padding-left: 8px;\n}") != std::string::npos,
           "the new declaration lands inside the rule, got `" + applied + "`");
}

void test_class_rejects_a_file_the_project_lacks() {
    PatchContext context = context_for(kNoStyleSource);
    point_at_view(context, kNoStyleSource);
    PatchRequest request;
    request.destination = PatchDestination::Class;
    request.class_name = "card";
    request.file = "ui/Missing.css";
    request.declarations = {{"color", "red"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(reason_of(patch) == "no_such_class_file",
           "a class commit naming a file the project lacks is `no_such_class_file`");
}

void test_token_rejects_a_literal_override() {
    PatchContext context = context_for(kInlineSource);
    point_at_view(context, kInlineSource);
    PatchRequest request;
    request.destination = PatchDestination::Token;
    request.token = "--accent";
    request.token_value = "#ff0000";
    // A literal value is not bound to any token, so there is nothing for this destination to edit.
    request.declarations = {{"color", "red"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(reason_of(patch) == "not_token_bound",
           "a token commit whose override is not `var(...)` is `not_token_bound`");
}

void test_token_edits_the_existing_declaration() {
    PatchContext context = context_for(kInlineSource);
    point_at_view(context, kInlineSource);
    PatchRequest request;
    request.destination = PatchDestination::Token;
    request.token = "--accent";
    request.token_value = "#ff0000";
    request.declarations = {{"color", "var(--accent)"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(patch.has_value(), "a bound token commit produces a patch, got `" +
                                  std::string(reason_of(patch)) +
                                  "`: " + (patch ? std::string() : patch.error().detail));
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 1, "a token commit is one edit");
    if (patch->edits.size() != 1) {
        return;
    }
    const TextEdit& edit = patch->edits.front();
    expect(edit.file == "assets.theme", "the edit names the theme asset");
    expect(kTheme.substr(edit.offset, edit.length) == "\"#3366ff\"",
           "the edit replaces exactly the token's value, got `" +
               std::string(kTheme.substr(edit.offset, edit.length)) + "`");
    expect(edit.new_text == "\"#ff0000\"", "the new value is the commit's");
    const std::string applied = apply_in_order(std::string(kTheme), patch->edits);
    expect(applied.find("\"--surface\": \"#ffffff\"") != std::string::npos,
           "the other token in the same theme is untouched");
    expect(applied.find("\"dark\"") != std::string::npos &&
               applied.find("\"#88aaff\"") != std::string::npos,
           "the other theme's token is untouched");
}

void test_token_rejects_a_token_the_theme_lacks() {
    PatchContext context = context_for(kInlineSource);
    point_at_view(context, kInlineSource);
    PatchRequest request;
    request.destination = PatchDestination::Token;
    request.token = "--missing";
    request.token_value = "#ff0000";
    request.declarations = {{"color", "var(--missing)"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(reason_of(patch) == "not_token_bound",
           "a token the active theme does not define is a rejection, not an append");
}

void test_no_such_node_and_no_overrides() {
    PatchContext missing = context_for(kInlineSource);
    point_at_view(missing, kInlineSource);
    missing.node_found = false;
    expect(reason_of(cli::build_source_patch(inline_request(), missing)) == "no_such_node",
           "a NodeRef that names no node is `no_such_node`");

    PatchContext empty = context_for(kInlineSource);
    point_at_view(empty, kInlineSource);
    PatchRequest request = inline_request();
    request.declarations.clear();
    expect(reason_of(cli::build_source_patch(request, empty)) == "no_overrides",
           "a node with no overrides is `no_overrides`");
}

void test_component_invalid_without_a_span() {
    PatchContext context = context_for(kInlineSource);
    // No span: the node came from a source the front end could not locate.
    expect(reason_of(cli::build_source_patch(inline_request(), context)) == "component_invalid",
           "a node with no source span is `component_invalid`");
}

void test_stale_source_without_a_version() {
    PatchContext context = context_for(kInlineSource);
    point_at_view(context, kInlineSource);
    context.versions.clear();
    expect(reason_of(cli::build_source_patch(inline_request(), context)) == "stale_source",
           "a file the viewer holds no version of is `stale_source`");
}

// The ordering invariant, asserted as a property rather than as an order. The patch describes ONE
// transformation of the original file, so applying the list in the order given must produce exactly
// the text that splicing every range against the original offsets produces. That is the property
// the spec's "sorted by descending offset" rule exists to guarantee: with an ascending list, the
// first edit would shift the second and the two results would differ.
void test_descending_order_is_application_order_independent() {
    PatchContext context = context_for(kNoStyleSource, kStylesheet);
    point_at_view(context, kNoStyleSource);
    PatchRequest request;
    request.destination = PatchDestination::Class;
    request.class_name = "card";
    request.file = "ui/Card.css";
    // Two declarations, so the rule produces two edits in one file, and their new texts differ in
    // length from what they replace - which is what makes the ordering observable at all.
    request.declarations = {{"color", "red"}, {"padding-left", "8px"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(patch.has_value(), "a two-declaration commit produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->edits.size() == 2, "two declarations produce two edits");
    if (patch->edits.size() != 2) {
        return;
    }
    // The list is sorted by file and DESCENDING offset.
    expect(patch->edits[0].offset > patch->edits[1].offset,
           "the edits are sorted by descending offset");
    // Non-overlapping.
    const TextEdit& high = patch->edits[0];
    const TextEdit& low = patch->edits[1];
    expect(static_cast<std::uint64_t>(low.offset) + low.length <= high.offset,
           "the edits do not overlap");
    // At least one edit changes the file's length, which is what makes the ordering rule
    // load-bearing: with every replacement the same length as its range, ascending application
    // would happen to work and the rule would be decorative. Here it does not - applying ascending
    // would shift the second edit by the first one's delta and corrupt the file.
    expect(high.new_text.size() != high.length || low.new_text.size() != low.length,
           "at least one edit changes the file's length, so the ordering rule is load-bearing");
    // The property: applying in the given order equals the one simultaneous transformation.
    const std::string forward = apply_in_order(std::string(kStylesheet), patch->edits);
    const std::string reference = apply_simultaneously(kStylesheet, patch->edits);
    expect(forward == reference,
           "applying the edits in the given order gives the same file as splicing them at once");
    expect(forward.find(".card {\n  color: red;\n  padding-left: 8px;\n}") != std::string::npos,
           "both declarations land in the rule, got `" + forward + "`");
}

void test_base_versions_cover_every_affected_file() {
    PatchContext context = context_for(kNoStyleSource, kStylesheet);
    point_at_view(context, kNoStyleSource);
    PatchRequest request;
    request.destination = PatchDestination::Class;
    request.class_name = "card";
    request.file = "ui/Card.css";
    request.declarations = {{"color", "red"}};
    const auto patch = cli::build_source_patch(request, context);
    expect(patch.has_value(), "the commit produces a patch");
    if (!patch) {
        return;
    }
    expect(patch->base_versions.size() == 1, "one affected file gives one base version");
    if (patch->base_versions.size() != 1) {
        return;
    }
    expect(patch->base_versions.front().first == "ui/Card.css",
           "the base version names the edited file");
    expect(patch->base_versions.front().second == 1, "the base version is the one the viewer held");
}

} // namespace

int main() {
    test_inline_replaces_the_style_attribute();
    test_inline_inserts_a_missing_style_attribute();
    test_inline_range_is_exact_on_a_unicode_source();
    test_class_appends_a_missing_rule();
    test_class_edits_an_existing_rule();
    test_class_inserts_a_declaration_the_rule_lacks();
    test_class_rejects_a_file_the_project_lacks();
    test_token_rejects_a_literal_override();
    test_token_edits_the_existing_declaration();
    test_token_rejects_a_token_the_theme_lacks();
    test_no_such_node_and_no_overrides();
    test_component_invalid_without_a_span();
    test_stale_source_without_a_version();
    test_descending_order_is_application_order_independent();
    test_base_versions_cover_every_affected_file();
    if (failures() != 0) {
        std::cerr << "scene_canvas_patch: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_patch: ok\n";
    return 0;
}
