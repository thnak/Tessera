// SPDX-License-Identifier: MIT
//
// The i18n service (docs/specs/i18n-v1.md sections 1, 5 and 8.1; ADR-050 sections 3 and 6; issue
// #195). It owns the one piece of state a locale change is about - the resolved locale - and
// publishes it through AF4's `Observable<T>` so a component, a binding or a formatting cache hears
// about a change once per flush instead of polling.
//
// What the spec fixes, and what this service therefore does:
//
//   * **Resolution is a chain, not a lookup** (section 8.1): each request in priority order walks
//     `exact -> language+script -> language`, and a total miss lands on the table's fallback, then
//     its default. `LocaleTable` owns that chain; the service only decides *which* requests to
//     submit (the app default now, a per-window override in front of it).
//   * **A change increments `locale_epoch`** (section 8.1). `epoch` is monotonic and increments
//   only
//     when the resolved tag or the seeded direction actually changes, which is what makes an
//     unchanged request a no-op: `set_requests` with the same request does not advance the epoch
//     and does not wake the scheduler, so a host that forwards a settings panel's every keystroke
//     cannot relayout on every key.
//   * **The locale seeds the base direction** (section 4): the resolved direction is what the app
//     writes into the root's inherited `direction` property. The service exposes it rather than
//     writing stylesheet state itself, because a service must not touch scene nodes (services.md
//     section 1).
//   * **No OS locale, no `setlocale`, no ICU probe.** The supported set and each locale's direction
//     come from the compiled table (i18n-v1.md section 8.1: "ICU data is ... pinned; locale packs
//     are bundled"). That is the spec's deterministic mode applied to every build: the same
//     requests produce the same resolution on every machine, which is also what the acceptance's
//     "tests are deterministic" requires.
//
// Deliberately not in this slice (reported as the next slice, with the exact missing pieces):
// number/date/currency/relative-time formatting (section 3), ICU MessageFormat plural/select
// (section 2), locale-aware parsers (section 5), the `en-XA`/`ar-XB` pseudo-locales (section 8.1),
// persistence of the user override through AF7's settings store, and accessibility announcements.

#pragma once

#include "tessera/appfw/i18n_catalog.hpp"
#include "tessera/appfw/i18n_locale.hpp"
#include "tessera/appfw/observable.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// The published value. Equality is deliberately *not* the whole struct: it is "the same resolved
// locale", so `Observable` coalesces a redundant request. The epoch is carried for cache
// invalidation and is monotonic per actual change (i18n-v1.md section 8.1).
struct LocaleSnapshot {
    std::string tag;
    TextDirection direction = TextDirection::LeftToRight;
    std::uint64_t epoch = 0;

    [[nodiscard]] bool operator==(const LocaleSnapshot& other) const noexcept {
        return tag == other.tag && direction == other.direction;
    }
};

// Everything the service needs, all of it explicit data (services.md section 1: "Configuration is
// explicit"). Both tables are moved into the service; a caller that wants to keep them passes
// copies.
struct I18nServiceConfig {
    LocaleTable locales;
    MessageCatalog catalog;
    // The requests the app starts with, in priority order (user override, app default, OS list).
    // Borrowed only for the duration of the constructor.
    std::vector<std::string> initial_requests;
};

// One per app scope. The scheduler must outlive the service: the observable attaches a ticket to it
// and detaches in its destructor. Non-movable, like `Observable<T>` itself - the ticket's address
// is part of the scheduler's state.
class LocaleService {
public:
    LocaleService(I18nServiceConfig config, ui::UpdateScheduler& scheduler);

    LocaleService(const LocaleService&) = delete;
    LocaleService& operator=(const LocaleService&) = delete;
    LocaleService(LocaleService&&) = delete;
    LocaleService& operator=(LocaleService&&) = delete;

    // Applies a new app-level request list, in priority order. Returns true when the resolved
    // locale or its direction changed (the epoch advanced). False means "same locale": nothing was
    // dirtied.
    bool set_requests(const std::vector<std::string>& requests);

    // The per-window override the spec allows (sections 1 and 8.1): explicit, cleared by an empty
    // request, and submitted *in front of* the app-level list. Returns true when the locale
    // changed.
    bool set_window_override(std::string_view request);
    [[nodiscard]] std::string_view window_override() const noexcept {
        return window_override_;
    }

    // The message for `id` in the current locale, through the catalog's fallback chain. A miss
    // returns the visible key and records a bounded diagnostic - never empty text (section 8.1).
    [[nodiscard]] std::string_view text(std::string_view id) const;

    [[nodiscard]] const LocaleSnapshot& current() const noexcept {
        return current_;
    }
    [[nodiscard]] const LocaleResolution& resolution() const noexcept {
        return resolution_;
    }
    [[nodiscard]] std::uint64_t epoch() const noexcept {
        return current_.epoch;
    }
    [[nodiscard]] TextDirection direction() const noexcept {
        return current_.direction;
    }
    [[nodiscard]] bool right_to_left() const noexcept {
        return current_.direction == TextDirection::RightToLeft;
    }

    [[nodiscard]] const LocaleTable& locales() const noexcept {
        return locales_;
    }
    [[nodiscard]] const MessageCatalog& catalog() const noexcept {
        return catalog_;
    }
    [[nodiscard]] MessageCatalog& catalog() noexcept {
        return catalog_;
    }

    // The change signal. `subscribe` allocates at mount, never per change; a flush delivers one
    // notification per changed locale, and `Observable::version()` counts accepted changes.
    [[nodiscard]] Observable<LocaleSnapshot>& changed() noexcept {
        return changed_;
    }
    [[nodiscard]] const Observable<LocaleSnapshot>& changed() const noexcept {
        return changed_;
    }

private:
    // The request list the table sees: the window override first (the most specific request the
    // spec defines), then the app-level list in the order the caller gave it.
    [[nodiscard]] std::vector<std::string_view> request_views() const;

    // Walks the current request list and updates `current_`/`resolution_`; returns whether the
    // resolved locale state changed.
    bool resolve_current();

    // The constructor's first resolution. A static helper because `resolution_` must be initialized
    // before `changed_` can be constructed with it, and member init order is declaration order.
    [[nodiscard]] static LocaleResolution resolve_requests(const LocaleTable& locales,
                                                           const std::vector<std::string>& requests,
                                                           std::string_view window_override);

    LocaleTable locales_;
    MessageCatalog catalog_;
    std::vector<std::string> requests_;
    std::string window_override_;
    LocaleResolution resolution_;
    LocaleSnapshot current_;
    // Constructed with the resolved snapshot, so the first value a subscriber sees is a supported
    // locale rather than an empty one.
    Observable<LocaleSnapshot> changed_;
};

} // namespace tessera::appfw
