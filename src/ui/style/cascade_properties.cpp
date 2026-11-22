// SPDX-License-Identifier: MIT
//
// The effective-style view of a computed style (issue #735, slice F).
//
// A computed style stores only its delta over the registry's shared initial values, but every
// reader
// - layout, paint, the animation bridge, the codegen inspector - wants the *effective* style: the
// delta merged with those defaults, sorted by name. Materializing that would put the ~100-entry
// table back on every read, so `properties()` returns this view instead: a two-range merge that
// allocates nothing and yields views into the delta and the defaults table, both of which outlive
// it.

#include "tessera/ui/style/cascade.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::style {

ComputedStyle::Properties::const_iterator& ComputedStyle::Properties::const_iterator::operator++() {
    advance();
    return *this;
}

void ComputedStyle::Properties::const_iterator::advance() {
    const std::size_t delta_size = delta_ != nullptr ? delta_->size() : 0;
    const std::size_t default_size =
        defaults_ != nullptr ? static_cast<std::size_t>(defaults_->size()) : 0;
    while (di_ < delta_size || si_ < default_size) {
        const std::string_view delta_name =
            di_ < delta_size ? std::string_view((*delta_)[di_].first) : std::string_view{};
        // A default with no initial value is absent until declared, so it is not part of the
        // effective style; skip it without materialising anything.
        while (si_ < default_size && defaults_->initial_of(si_) == nullptr) {
            ++si_;
        }
        const std::string_view default_name =
            si_ < default_size ? defaults_->name_of(si_) : std::string_view{};
        if (di_ < delta_size && (si_ >= default_size || delta_name < default_name)) {
            current_ = Entry{delta_name, (*delta_)[di_].second};
            ++di_;
        } else if (si_ < default_size && (di_ >= delta_size || default_name < delta_name)) {
            current_ = Entry{default_name, *defaults_->initial_of(si_)};
            ++si_;
        } else {
            // The delta wins on a name it holds; the default is the same property.
            current_ = Entry{delta_name, (*delta_)[di_].second};
            ++di_;
            ++si_;
        }
        done_ = false;
        return;
    }
    // Normalise the exhausted position, so every exhausted iterator compares equal to `end()`
    // whatever order it walked the two ranges in.
    di_ = delta_size;
    si_ = static_cast<std::uint16_t>(default_size);
    done_ = true;
}

std::size_t ComputedStyle::Properties::size() const {
    std::size_t count = 0;
    for (const_iterator it = begin(); it != end(); ++it) {
        ++count;
    }
    return count;
}

bool ComputedStyle::Properties::operator==(const Properties& other) const {
    const_iterator left = begin();
    const_iterator right = other.begin();
    const const_iterator left_end = end();
    const const_iterator right_end = other.end();
    while (left != left_end && right != right_end) {
        if (*left != *right) {
            return false;
        }
        ++left;
        ++right;
    }
    return left == left_end && right == right_end;
}

} // namespace tessera::ui::style
