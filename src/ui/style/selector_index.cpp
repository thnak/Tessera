// SPDX-License-Identifier: MIT

// The selector index (stylesheet-v1 section 14): id, class, tag and `::part()` buckets that narrow
// the candidate rules for a scene node before matching. Split from selector_match.cpp (the
// matching) to respect the file limits.

#include "tessera/ui/style/selector_match.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace tessera::ui::style {

void SelectorIndex::add(const ComplexSelector& selector, std::size_t value) {
    add_entry(Entry{selector, value, specificity(selector)});
}

void SelectorIndex::add(const SelectorList& selectors, std::size_t value) {
    for (const ComplexSelector& selector : selectors.selectors) {
        add(selector, value);
    }
}

void SelectorIndex::clear() {
    entries_.clear();
    id_index_.clear();
    class_index_.clear();
    tag_index_.clear();
    part_index_.clear();
    universal_index_.clear();
    candidates_.clear();
    seen_.clear();
    stamp_ = 0;
}

void SelectorIndex::add_entry(Entry entry) {
    if (entry.selector.compounds.empty()) {
        return;
    }
    const std::size_t index = entries_.size();
    const CompoundSelector& subject = entry.selector.compounds.back();
    entries_.push_back(std::move(entry));

    // A `::part()` subject is bucketed by its first part name: the compound's tag/class/id describe
    // the host, so they are not attributes of the node being matched.
    const std::size_t part_index = part_pseudo_index(subject);
    if (part_index != subject.pseudos.size()) {
        part_index_[std::string(part_name_at(subject.pseudos[part_index].argument, 0))].push_back(
            index);
    } else if (!subject.id.empty()) {
        id_index_[subject.id].push_back(index);
    } else if (!subject.classes.empty()) {
        class_index_[subject.classes.front()].push_back(index);
    } else if (!subject.tag.empty()) {
        tag_index_[subject.tag].push_back(index);
    } else {
        universal_index_.push_back(index);
    }
}

void SelectorIndex::match(const StyleNode& node, std::vector<SelectorMatch>& out) const {
    out.clear();
    candidates_.clear();
    // A stamp instead of a cleared `vector<bool>`: `seen_` is sized once and each match bumps the
    // stamp, so the per-node cost is proportional to the candidates examined rather than to the
    // number of indexed selectors (security review finding P3).
    if (seen_.size() < entries_.size()) {
        seen_.resize(entries_.size(), 0);
    }
    if (++stamp_ == 0) {
        // Wrapped: every stored stamp is stale, so start over rather than alias an old one.
        std::fill(seen_.begin(), seen_.end(), 0);
        stamp_ = 1;
    }

    const auto append = [&](std::size_t index) {
        if (seen_[index] != stamp_) {
            seen_[index] = stamp_;
            candidates_.push_back(index);
        }
    };

    if (!node.id.empty()) {
        const auto found = id_index_.find(std::string(node.id));
        if (found != id_index_.end()) {
            for (const std::size_t index : found->second) {
                append(index);
            }
        }
    }
    for (const std::string_view name : node.classes) {
        const auto found = class_index_.find(std::string(name));
        if (found != class_index_.end()) {
            for (const std::size_t index : found->second) {
                append(index);
            }
        }
    }
    for (std::size_t part = 0;; ++part) {
        const std::string_view name = part_name_at(node.part, part);
        if (name.empty()) {
            break;
        }
        const auto found = part_index_.find(std::string(name));
        if (found != part_index_.end()) {
            for (const std::size_t index : found->second) {
                append(index);
            }
        }
    }
    if (!node.tag.empty()) {
        const auto found = tag_index_.find(std::string(node.tag));
        if (found != tag_index_.end()) {
            for (const std::size_t index : found->second) {
                append(index);
            }
        }
    }
    if (!node.alias_tag.empty() && node.alias_tag != node.tag) {
        const auto found = tag_index_.find(std::string(node.alias_tag));
        if (found != tag_index_.end()) {
            for (const std::size_t index : found->second) {
                append(index);
            }
        }
    }
    for (const std::size_t index : universal_index_) {
        append(index);
    }

    for (const std::size_t index : candidates_) {
        const Entry& entry = entries_[index];
        if (!matches(entry.selector, node)) {
            continue;
        }
        // The candidate list is deduplicated by entry index, so two entries can still carry the
        // same rule handle (a selector list). Keep the highest specificity, as before.
        auto found = std::find_if(out.begin(), out.end(), [&](const SelectorMatch& result) {
            return result.value == entry.value;
        });
        if (found == out.end()) {
            out.push_back(SelectorMatch{entry.value, entry.specificity});
        } else if (entry.specificity > found->specificity) {
            found->specificity = entry.specificity;
        }
    }
}

std::vector<SelectorMatch> SelectorIndex::match(const StyleNode& node) const {
    std::vector<SelectorMatch> out;
    match(node, out);
    return out;
}

} // namespace tessera::ui::style
