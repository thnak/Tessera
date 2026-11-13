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

std::vector<SelectorMatch> SelectorIndex::match(const StyleNode& node) const {
    std::vector<std::size_t> candidates;
    std::vector<bool> seen(entries_.size(), false);

    const auto append = [&](std::size_t index) {
        if (!seen[index]) {
            seen[index] = true;
            candidates.push_back(index);
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
    for (const std::size_t index : universal_index_) {
        append(index);
    }

    std::vector<SelectorMatch> matches_out;
    for (const std::size_t index : candidates) {
        const Entry& entry = entries_[index];
        if (!matches(entry.selector, node)) {
            continue;
        }
        auto found =
            std::find_if(matches_out.begin(), matches_out.end(),
                         [&](const SelectorMatch& result) { return result.value == entry.value; });
        if (found == matches_out.end()) {
            matches_out.push_back(SelectorMatch{entry.value, entry.specificity});
        } else if (entry.specificity > found->specificity) {
            found->specificity = entry.specificity;
        }
    }
    return matches_out;
}

} // namespace tessera::ui::style
