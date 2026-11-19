// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The designer's override layer (designer-protocol-v1 sections 9.2-9.4): the values a developer
// tried on a node in the studio before deciding to commit them to source.
//
// **What this file is.** The layer's *data*: which node carries which property/value, in the order
// they were set, plus the undo/redo history of the operations that changed it. It never touches a
// tree and never evaluates CSS beyond validating one declaration; `PreviewSession` is what applies
// the entries to the live scene, and `CanvasSceneSession` is what the page talks to. Keeping the
// layer a plain value is what lets its history rules be asserted without a running evaluation.
//
// **Why it lives beside the session and not inside the scene.** A tree is rebuilt after every
// dispatch that changes state and after every edit that evaluates, and the layer has to survive
// both: an override the developer set must not vanish because they clicked a button. The layer is
// owned by the canvas session, outlives each `PreviewSession`, and is re-applied to every tree the
// session builds.
namespace tessera::compiler::codegen::cli {

// One property a node carries in the override layer, with the value as the developer typed it.
struct StyleOverride {
    std::uint64_t automation_id = 0;
    std::string property;
    std::string value;

    bool operator==(const StyleOverride&) const = default;
};

// One reason a declaration was refused: a `TESSERA_CSS_*` code from the stylesheet engine
// (stylesheet-v1 section 15), or `TESSERA_DESIGNER_LIMIT` for a declaration the scene cannot hold
// (designer-protocol-v1 section 12).
struct OverrideDiagnostic {
    std::string code;
    std::string message;
};

// Checks one declaration with the stylesheet engine, the code the compiler uses
// (designer-protocol-v1 section 9.2): the declaration is prepared as a one-rule sheet, so an
// unknown property, a value the property rejects and a shorthand that does not expand are all
// reported with the spelling the compiler would give them. Returns nullopt when it is valid.
//
// A value that would end the declaration early (`;`) or leave the rule (`}`) is refused as a parse
// error rather than handed to the parser, because the one-rule sheet is built by concatenation and
// a value must not be able to add a second declaration or a second rule to what is being checked.
[[nodiscard]] std::optional<OverrideDiagnostic> validate_override(std::string_view property,
                                                                  std::string_view value);

// The override layer and its history.
//
// **History is snapshots, not inverse operations.** `set`, `clear` and `revert` each push the
// entries as they were before the change; `undo` swaps the entries with the newest snapshot and
// `redo` swaps back. An inverse-operation log would need one inverse per operation kind (a `clear`
// of one property, a `clear` of all of a node's, a `revert` of every node), and the first one it
// got wrong would restore a layer the developer never had. A snapshot is the layer, so restoring it
// cannot be wrong. The layer holds at most a few dozen entries, so a snapshot is cheap, and the
// stack is bounded at `kMaxHistory`.
class OverrideLayer {
public:
    static constexpr std::size_t kMaxHistory = 128;

    // Sets `property` of `automation_id` to `value`, replacing a value already set for it. The
    // declaration must already be validated. Returns false (and records nothing) when the layer
    // already holds exactly this value, so setting the same thing twice is one undo step, not two.
    bool set(std::uint64_t automation_id, std::string_view property, std::string_view value);

    // Removes one property of a node, or every property of it when `property` is empty. Returns
    // false when there was nothing to remove.
    bool clear(std::uint64_t automation_id, std::string_view property = {});

    // Removes every override of every node.
    bool revert();

    // Undo and redo of the last `set`/`clear`/`revert`. False when the stack is empty.
    bool undo();
    bool redo();

    // The commit consumed this node's overrides (designer-protocol-v1 section 9.4): they leave the
    // layer, and the history forgets them too, so an `edit.undo` after a commit cannot bring back
    // a value that is now in the developer's file. Snapshots that differ only by this node's
    // entries collapse into the one before them. Not undoable itself.
    void consume(std::uint64_t automation_id);

    [[nodiscard]] const std::vector<StyleOverride>& entries() const noexcept {
        return entries_;
    }
    // The node's overrides in the order they were set; the declaration list a commit writes.
    [[nodiscard]] std::vector<StyleOverride> of(std::uint64_t automation_id) const;
    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty();
    }
    [[nodiscard]] bool can_undo() const noexcept {
        return !undo_.empty();
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return !redo_.empty();
    }

private:
    using Snapshot = std::vector<StyleOverride>;

    void push_history();

    std::vector<StyleOverride> entries_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

} // namespace tessera::compiler::codegen::cli
