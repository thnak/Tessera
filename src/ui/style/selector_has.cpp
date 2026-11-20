// SPDX-License-Identifier: MIT

// `:has(<relative selector list>)` matching (issue #622). Split from selector_match.cpp (the
// matcher) so each file stays under the source-size limit: this is the one selector form that
// looks *down*, so its walk and its cost bound live together.

#include "tessera/ui/style/selector_match.hpp"

#include <algorithm>

namespace tessera::ui::style {

// `:has(<relative selector list>)` (issue #622): the argument is matched against the node's
// descendants and its following siblings (and their descendants), which is the CSS Scoping 1
// relative-selector anchor set for a non-`:scope` context. The argument keeps its own combinators,
// so
// `:has(> .icon)` is a child and `:has(.icon)` is any descendant.
//
// This is the one selector form that looks *down*, so it is the one whose cost is not a function of
// the node alone: the walk is bounded by the subtree (and the following-sibling subtrees) it
// visits. The selector index still narrows the candidate rules by their subject compound first, so
// a rule without `:has()` never walks a subtree and a rule with it is filtered before the walk
// runs.
bool subtree_matches(const SelectorList& list, const StyleNode& node) {
    if (matches(list, node)) {
        return true;
    }
    for (const StyleNode* child : node.children) {
        if (child != nullptr && subtree_matches(list, *child)) {
            return true;
        }
    }
    return false;
}

bool match_has(const PseudoClass& pseudo, const StyleNode& node) {
    if (pseudo.nested == nullptr) {
        return false;
    }
    for (const StyleNode* child : node.children) {
        if (child != nullptr && subtree_matches(*pseudo.nested, *child)) {
            return true;
        }
    }
    if (node.parent == nullptr) {
        return false;
    }
    // Following siblings, and their descendants: a relative selector is anchored at the node, so a
    // sibling that matches the argument makes `:has()` true.
    const auto& siblings = node.parent->children;
    const auto position = std::find(siblings.begin(), siblings.end(), &node);
    if (position == siblings.end()) {
        return false;
    }
    for (auto it = position + 1; it != siblings.end(); ++it) {
        if (*it != nullptr && subtree_matches(*pseudo.nested, **it)) {
            return true;
        }
    }
    return false;
}

bool matches_has(const PseudoClass& pseudo, const StyleNode& node) {
    return match_has(pseudo, node);
}

} // namespace tessera::ui::style
