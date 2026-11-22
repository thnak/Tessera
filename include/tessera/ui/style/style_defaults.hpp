// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

// One registered longhand: its name, its CSS initial value (empty when the property is absent until
// declared, as the animation and transition names are) and whether it inherits.
struct StyleDefault {
    std::string_view name;
    std::string_view initial;
    bool inherited = false;
};
// The registry's initial values, shared by every computed style it computes (issue #735, slices E
// and F).
//
// Slice F: a computed style stores only the entries that differ from this table, so a node's style
// is a handful of entries instead of ~100 and the style tree's memo holds kilobytes instead of
// megabytes. Slice E: every registered longhand gets a stable small id, so reading a default is one
// id-indexed array read instead of a binary search over the whole style with a `memcmp` per step -
// which `perf` put at 53% of a size-change frame.
//
// One table per registry, built once from the registry's own merged property list, so it cannot
// drift from what the registry accepts. It is immutable and held by `shared_ptr`, so a style can
// outlive the registry that computed it (the style tree's memo keeps styles across frames).
class StyleDefaults {
public:
    // `definitions` is every registered longhand with its initial value and inheritance flag, in
    // any order; ids are assigned in sorted-name order so they are stable across builds.
    explicit StyleDefaults(std::vector<StyleDefault> definitions);

    // The initial value of `name`, or nullptr when the property has none: absent until declared
    // (the animation and transition names), or not a registered longhand at all (a custom
    // property).
    [[nodiscard]] const std::string* find(std::string_view name) const;

    // The stable small id of a registered longhand, or `kNoId`. Ids are dense and assigned in
    // sorted-name order, so a reader that holds one never compares strings again.
    [[nodiscard]] std::uint16_t id(std::string_view name) const;
    [[nodiscard]] std::string_view name_of(std::uint16_t id) const;
    [[nodiscard]] const std::string* initial_of(std::uint16_t id) const;

    // How many longhands carry an initial value: the size of the effective style a delta expands
    // to.
    [[nodiscard]] std::size_t size() const {
        return slots_.size();
    }

    static constexpr std::uint16_t kNoId = 0xFFFFU;

private:
    struct Slot {
        // Owned, not a view: the definitions come from a registry's own map, which dies with the
        // registry, while this table outlives it (a style can outlive the sheet that computed it).
        std::string name;
        std::string initial;
    };
    // Indexed by id: `initial_of` is one bounds check and no comparison.
    std::vector<Slot> slots_;
    // Name to id, open-addressed. A default read is the hot path of a style lookup, so it is one
    // probe rather than a binary search over every registered longhand. The keys view `slots_`,
    // which is stable once the constructor returns.
    std::vector<std::pair<std::string_view, std::uint16_t>> index_;
};

} // namespace tessera::ui::style
