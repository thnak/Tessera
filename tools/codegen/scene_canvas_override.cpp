// SPDX-License-Identifier: MIT

// The canvas session's override operations (issue #536, designer-protocol-v1 sections 9.2-9.4 and
// 10.3). The data and its history are `OverrideLayer` (`scene_override.hpp`); how a layer reaches a
// tree is `PreviewSession::resync_overrides()`. This unit is the glue the page calls: validate,
// change the layer, restyle, answer.

#include "scene_canvas_session.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;
namespace style = tessera::ui::style;

std::vector<StyleOverrideInfo> infos_of(const std::vector<StyleOverride>& entries) {
    std::vector<StyleOverrideInfo> result;
    result.reserve(entries.size());
    for (const StyleOverride& entry : entries) {
        result.push_back(StyleOverrideInfo{entry.automation_id, entry.property, entry.value});
    }
    return result;
}

OverrideOutcome refused(std::string reason) {
    OverrideOutcome outcome;
    outcome.reason = std::move(reason);
    return outcome;
}

} // namespace

std::vector<StyleOverrideInfo> CanvasSceneSession::overrides_of(std::uint64_t automation_id) const {
    return infos_of(overrides_.of(automation_id));
}

OverrideOutcome CanvasSceneSession::override_set(std::uint64_t automation_id,
                                                 std::string_view property,
                                                 std::string_view value) {
    const ui::Node* node = started() ? session_->find_by_automation_id(automation_id) : nullptr;
    if (node == nullptr) {
        return refused("no_such_node");
    }
    if (const std::optional<OverrideDiagnostic> diagnostic = validate_override(property, value)) {
        OverrideOutcome outcome;
        outcome.reason = "invalid_override";
        outcome.diagnostics.push_back(*diagnostic);
        outcome.overrides = overrides_of(automation_id);
        outcome.can_undo = overrides_.can_undo();
        outcome.can_redo = overrides_.can_redo();
        return outcome;
    }
    // The scene keeps an override as one of the node's bounded inline declarations. A property the
    // node does not already carry needs a free slot; refuse it here, with the layer untouched,
    // rather than apply an override that silently did not land.
    const std::string name = style::normalize_property(property);
    bool present = false;
    for (const ui::Node::InlineStyleDeclaration& declaration : node->inline_style()) {
        present = present || declaration.name == name;
    }
    if (!present && node->inline_style().size() >= ui::Node::kMaxInlineStyleDeclarations) {
        OverrideOutcome outcome;
        outcome.reason = "invalid_override";
        outcome.diagnostics.push_back(OverrideDiagnostic{
            "TESSERA_DESIGNER_LIMIT", "the node has no room for another override"});
        outcome.overrides = overrides_of(automation_id);
        outcome.can_undo = overrides_.can_undo();
        outcome.can_redo = overrides_.can_redo();
        return outcome;
    }
    OverrideOutcome outcome;
    outcome.ok = true;
    outcome.changed = overrides_.set(automation_id, property, value);
    if (outcome.changed) {
        (void)session_->resync_overrides();
    }
    outcome.overrides = overrides_of(automation_id);
    outcome.can_undo = overrides_.can_undo();
    outcome.can_redo = overrides_.can_redo();
    return outcome;
}

OverrideOutcome CanvasSceneSession::override_clear(std::uint64_t automation_id,
                                                   std::string_view property) {
    if (!started() || session_->find_by_automation_id(automation_id) == nullptr) {
        return refused("no_such_node");
    }
    OverrideOutcome outcome;
    outcome.ok = true;
    outcome.changed = overrides_.clear(automation_id, property);
    if (outcome.changed) {
        (void)session_->resync_overrides();
    }
    outcome.overrides = overrides_of(automation_id);
    outcome.can_undo = overrides_.can_undo();
    outcome.can_redo = overrides_.can_redo();
    return outcome;
}

OverrideOutcome CanvasSceneSession::override_revert() {
    OverrideOutcome outcome;
    outcome.ok = true;
    outcome.changed = overrides_.revert();
    if (outcome.changed && started()) {
        (void)session_->resync_overrides();
    }
    outcome.overrides = infos_of(overrides_.entries());
    outcome.can_undo = overrides_.can_undo();
    outcome.can_redo = overrides_.can_redo();
    return outcome;
}

OverrideOutcome CanvasSceneSession::edit_undo() {
    OverrideOutcome outcome;
    outcome.ok = true;
    outcome.changed = overrides_.undo();
    if (outcome.changed && started()) {
        (void)session_->resync_overrides();
    }
    outcome.overrides = infos_of(overrides_.entries());
    outcome.can_undo = overrides_.can_undo();
    outcome.can_redo = overrides_.can_redo();
    return outcome;
}

OverrideOutcome CanvasSceneSession::edit_redo() {
    OverrideOutcome outcome;
    outcome.ok = true;
    outcome.changed = overrides_.redo();
    if (outcome.changed && started()) {
        (void)session_->resync_overrides();
    }
    outcome.overrides = infos_of(overrides_.entries());
    outcome.can_undo = overrides_.can_undo();
    outcome.can_redo = overrides_.can_redo();
    return outcome;
}

CommitSettleResult CanvasSceneSession::settle_commit(std::uint64_t automation_id) {
    CommitSettleResult result;
    const ui::Node* node = started() ? session_->find_by_automation_id(automation_id) : nullptr;
    if (node == nullptr) {
        result.reason = "no_such_node";
        result.overrides = overrides_of(automation_id);
        return result;
    }
    const std::vector<StyleOverride> committed = overrides_.of(automation_id);
    if (committed.empty()) {
        result.ok = true;
        return result;
    }

    // The longhands the overrides touch, by the registry's own expansion, so a `padding` override
    // is compared on the four sides it sets.
    std::set<std::string> longhands;
    for (const StyleOverride& entry : committed) {
        style::Declaration declaration;
        declaration.property = entry.property;
        declaration.value = entry.value;
        for (const style::Declaration& expanded :
             sheet().registry().expand(declaration).declarations) {
            longhands.insert(expanded.property);
        }
    }
    const auto computed_values = [&]() {
        std::map<std::string, std::string> values;
        const ui::Node* current = session_->find_by_automation_id(automation_id);
        style::SceneStyleTree tree(session_->root());
        const style::ComputedStyle computed = tree.compute(sheet(), *current);
        for (const std::string& property : longhands) {
            const std::string* found = computed.find(property);
            values[property] = found != nullptr ? *found : std::string();
        }
        return values;
    };

    // With the overrides on, then off: what the new source must reproduce is what was on screen.
    const std::map<std::string, std::string> expected = computed_values();
    const OverrideLayer backup = overrides_;
    overrides_.consume(automation_id);
    (void)session_->resync_overrides();
    const std::map<std::string, std::string> actual = computed_values();

    if (actual == expected) {
        result.ok = true;
        result.consumed = true;
        return result;
    }
    // The committed edit did not produce the expected style: put the overrides back so the
    // developer can see the difference and commit again.
    overrides_ = backup;
    (void)session_->resync_overrides();
    std::string detail;
    for (const auto& [property, value] : expected) {
        if (actual.at(property) != value) {
            detail += detail.empty() ? "" : "; ";
            detail += property + " is `" + actual.at(property) + "`, expected `" + value + "`";
        }
    }
    result.reason = "patch_mismatch";
    result.diagnostics.push_back(OverrideDiagnostic{
        "TESSERA_DESIGNER_PATCH_MISMATCH",
        "the committed source does not give the node the overridden style: " + detail});
    result.overrides = overrides_of(automation_id);
    return result;
}

} // namespace tessera::compiler::codegen::cli
