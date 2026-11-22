// SPDX-License-Identifier: MIT

// The computed style's representation (issue #735, slices E and F): a style is a delta over the
// registry's shared initial values, and every registered longhand has a stable small id. These pin
// the contracts the rest of the engine reads through: `find` sees the effective value, `contains`
// sees only what the style holds itself, `properties()` still enumerates the full sorted effective
// style, and `identity`/`cached_digest` keep their meaning across the change.

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/layout_digest.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/style_defaults.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "style_delta: " << message << "\n";
        ++failures;
    }
}

void check_value(const style::ComputedStyle& computed, std::string_view property,
                 std::string_view expected, std::string_view message) {
    const std::string* value = computed.find(property);
    if (value == nullptr || *value != expected) {
        std::cerr << "style_delta: " << message << " (" << property << " = "
                  << (value == nullptr ? "<absent>" : *value) << ", expected " << expected << ")\n";
        ++failures;
    }
}

// How many registered longhands carry an initial value: the size of the effective style.
std::size_t initial_longhand_count(const style::PropertyRegistry& registry) {
    std::size_t count = 0;
    for (const std::string_view name : registry.longhand_names()) {
        const style::PropertyInfo* info = registry.find(name);
        if (info != nullptr && !info->initial.empty()) {
            ++count;
        }
    }
    return count;
}

// A style computed by the registry reads a default through `find` without holding it, and the delta
// stays empty: that is the whole point of slice F.
void test_defaults_are_not_stored() {
    const style::PropertyRegistry registry;
    const style::ComputedStyle initial = registry.initial_style();
    check(initial.delta().empty(), "an initial style holds no entries of its own");
    check(initial.properties().size() == initial_longhand_count(registry),
          "the effective style is every longhand with an initial value");
    check_value(initial, "display", "inline", "a default reads through find");
    check_value(initial, "color", "black", "an inherited default reads through find");
    // `contains` is the narrower question, and the cascade tests depend on it staying narrow.
    check(!initial.contains("display"), "contains does not report a default");
    check(!initial.contains("color"), "contains does not report an inherited default");
    // A longhand with no initial value is absent until declared, so it is not in the effective
    // style.
    check(initial.find("animation-name") == nullptr, "a longhand with no initial value is absent");
}

// A value equal to the default is not stored: the effective style is identical either way, and this
// is what keeps a node's style a handful of entries.
void test_redundant_writes_are_dropped() {
    const style::PropertyRegistry registry;
    style::ComputedStyle computed = registry.initial_style();
    computed.set("display", "inline");
    check(computed.delta().empty(), "writing the default value stores nothing");
    check_value(computed, "display", "inline", "the effective value is still the default");

    computed.set("display", "flex");
    check(computed.delta().size() == 1, "a differing value is stored");
    check_value(computed, "display", "flex", "the stored value wins over the default");

    computed.set("display", "inline");
    check(computed.delta().empty(), "writing the default back drops the entry");
    check_value(computed, "display", "inline", "the default is visible again");
}

// `properties()` is the effective style: sorted, complete, and equal to what a fully materialized
// style would have held before the change.
void test_properties_is_the_effective_style() {
    const style::PropertyRegistry registry;
    style::ComputedStyle computed = registry.initial_style();
    computed.set("display", "flex");
    computed.set("--accent", "purple");

    check(computed.properties().size() == initial_longhand_count(registry) + 1,
          "the effective style is the defaults plus the custom property");
    bool sorted = true;
    std::string_view previous;
    std::size_t seen = 0;
    for (const auto& [name, value] : computed.properties()) {
        (void)value;
        if (seen != 0 && !(previous < name)) {
            sorted = false;
        }
        previous = name;
        ++seen;
    }
    check(sorted, "the effective style is sorted by name");
    check(seen == computed.properties().size(), "the walk visits every entry");
    check_value(computed, "--accent", "purple", "a custom property is stored and found");
    check(computed.contains("--accent"), "a custom property is held by the style");
}

// The layout digest folds the effective style, so a default and an explicitly written default must
// hash the same: the layout gate compares digests across a restyle.
void test_layout_digest_sees_defaults() {
    const style::PropertyRegistry registry;
    style::ComputedStyle implicit = registry.initial_style();
    style::ComputedStyle explicit_default = registry.initial_style();
    explicit_default.set("width", "auto");
    check(style::layout_digest(implicit) == style::layout_digest(explicit_default),
          "a default and an explicit default hash the same");

    style::ComputedStyle changed = registry.initial_style();
    changed.set("width", "200px");
    check(style::layout_digest(implicit) != style::layout_digest(changed),
          "a differing layout value moves the digest");

    // A paint-only change must not move it.
    style::ComputedStyle painted = registry.initial_style();
    painted.set("background-color", "red");
    check(style::layout_digest(implicit) == style::layout_digest(painted),
          "a paint-only change keeps the digest");
}

// Copy-on-write still holds across the delta: a copy shares, the first write clones, and the digest
// lives with the shared entries.
void test_copy_on_write_with_defaults() {
    const style::PropertyRegistry registry;
    style::ComputedStyle original = registry.initial_style();
    original.set("width", "10px");
    style::ComputedStyle copy = original;
    check(copy.same_entries(original), "a copy shares the entries");
    original.cache_digest(42);
    check(copy.cached_digest() == 42, "the digest lives with the shared entries");

    copy.set("width", "20px");
    check(!copy.same_entries(original), "a write clones the shared entries");
    check_value(original, "width", "10px", "the original keeps its value");
    check_value(copy, "width", "20px", "the copy holds the new value");
    check(original.cached_digest() == 42, "the original keeps its digest");
    check(copy.cached_digest() == 0, "a clone has no cached digest");
    // The clone is still a delta over the same defaults, so a default read survives the clone.
    check_value(copy, "display", "inline", "a clone keeps the defaults it was computed against");
}

// Every registered longhand has a stable id, and the id round-trips to the same name and initial.
void test_ids_are_stable() {
    const style::PropertyRegistry registry;
    const auto& defaults = *registry.defaults();
    check(defaults.size() > 0, "the defaults table is populated");
    bool round_trips = true;
    for (std::uint16_t id = 0; id < defaults.size(); ++id) {
        const std::string_view name = defaults.name_of(id);
        if (name.empty() || defaults.id(name) != id) {
            round_trips = false;
        }
    }
    check(round_trips, "every id round-trips through its name");
    check(defaults.id("display") != style::StyleDefaults::kNoId, "a registered name has an id");
    check(defaults.id("--accent") == style::StyleDefaults::kNoId, "a custom property has no id");
    check(defaults.id("not-a-property") == style::StyleDefaults::kNoId,
          "an unknown name has no id");
    // The registry's own lookup and the defaults table must agree on what exists.
    check(registry.find("display") != nullptr && defaults.find("display") != nullptr,
          "the registry and the defaults agree on a registered longhand");
    check(registry.find("not-a-property") == nullptr && defaults.find("not-a-property") == nullptr,
          "the registry and the defaults agree on an unknown name");
}

// A style with no defaults (what `compute_style` returns) holds every entry itself, which is what
// the cascade tests pin: `initial`/`unset` leave nothing behind.
void test_bare_style_holds_everything() {
    style::ComputedStyle bare;
    bare.set("color", "red");
    check(bare.contains("color"), "a bare style holds what it was given");
    check_value(bare, "color", "red", "a bare style finds what it holds");
    check(bare.find("display") == nullptr, "a bare style has no defaults to fall back to");
    bare.erase("color");
    check(!bare.contains("color") && bare.find("color") == nullptr,
          "erasing a bare style leaves nothing");
}

} // namespace

int main() {
    test_defaults_are_not_stored();
    test_redundant_writes_are_dropped();
    test_properties_is_the_effective_style();
    test_layout_digest_sees_defaults();
    test_copy_on_write_with_defaults();
    test_ids_are_stable();
    test_bare_style_holds_everything();
    if (failures != 0) {
        std::cerr << "style_delta: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "style_delta: ok\n";
    return 0;
}
