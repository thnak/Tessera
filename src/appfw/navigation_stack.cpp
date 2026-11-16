// SPDX-License-Identifier: MIT
//
// The navigation controller (docs/specs/navigation-v1.md sections 2, 3, 8 and 10.1; issue #196).

#include "tessera/appfw/navigation_stack.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace tessera::appfw {
namespace {

RouteSnapshot snapshot_of(const std::vector<NavEntry>& entries, std::uint64_t epoch) {
    RouteSnapshot snapshot;
    if (!entries.empty()) {
        snapshot.entry = entries.back();
    }
    snapshot.depth = entries.size();
    snapshot.epoch = epoch;
    snapshot.at_root = entries.size() <= 1;
    return snapshot;
}

} // namespace

NavController::NavController(const RouteTable& table, ui::UpdateScheduler& scheduler,
                             NavControllerConfig config)
    : table_(&table),
      config_(config.capacity == 0 ? NavControllerConfig{} : config),
      route_(scheduler, snapshot_of(entries_, 0)) {
    entries_.reserve(config_.capacity);
}

NavController::~NavController() = default;

const NavEntry* NavController::active() const noexcept {
    return entries_.empty() ? nullptr : &entries_.back();
}

std::expected<NavEntry, NavigationError> NavController::entry_for_path(std::string_view path) {
    auto match = table_->match(path);
    if (!match.has_value()) {
        // Section 3: "Unknown route -> not-found screen with back/home actions." The controller
        // does not invent that screen; it reports `NotFound` and the caller routes it to
        // `RouteTable::not_found_route()`, which is a table decision. Nothing is recorded here, so
        // section 5's "never navigate to a half state" holds by construction.
        return std::unexpected(match.error());
    }
    const RouteDef* route = table_->at(match->route);
    if (route == nullptr) {
        return std::unexpected(
            NavigationError{NavigationErrorCode::UnknownRouteId, match->route_id, 0});
    }
    NavEntry entry;
    entry.entry_id = next_entry_id();
    entry.route_id = match->route_id;
    entry.params = std::move(match->params);
    entry.presentation = route->presentation;
    return entry;
}

std::expected<NavEntry, NavigationError> NavController::entry_for_route(std::string_view route_id,
                                                                        ParamMap params) {
    const RouteDef* route = table_->find(route_id);
    if (route == nullptr) {
        return std::unexpected(
            NavigationError{NavigationErrorCode::UnknownRouteId, std::string(route_id), 0});
    }
    // A push by id skips pattern matching, so the params arrive as given. They are still validated
    // against the declarations, so `push_route("orders.detail", {{"id","x"}})` fails exactly where
    // `push("/orders/x")` would.
    NavEntry entry;
    entry.entry_id = next_entry_id();
    entry.route_id = route->id;
    entry.params = std::move(params);
    entry.presentation = route->presentation;
    return entry;
}

bool NavController::is_duplicate(const NavEntry& candidate) const {
    const NavEntry* top = active();
    return top != nullptr && top->route_id == candidate.route_id && top->params == candidate.params;
}

std::expected<PushOutcome, NavigationError> NavController::record(NavEntry entry,
                                                                  bool replace_top) {
    const RouteDef* route = table_->find(entry.route_id);
    const DuplicatePolicy policy = route == nullptr ? DuplicatePolicy::Ignore : route->duplicates;
    // Section 2: "push same route with same params -> focus/no-op or replace, explicit per route."
    if (is_duplicate(entry)) {
        if (policy == DuplicatePolicy::Ignore) {
            return PushOutcome::DuplicateIgnored;
        }
        entries_.back() = std::move(entry);
        touch();
        return PushOutcome::Committed;
    }
    if (replace_top) {
        // Section 2's `replace`: the active entry is replaced and history does not grow. With an
        // empty stack there is nothing to replace, so the entry becomes the root rather than being
        // dropped.
        if (entries_.empty()) {
            entries_.push_back(std::move(entry));
        } else {
            entries_.back() = std::move(entry);
        }
        touch();
        return PushOutcome::Committed;
    }
    if (entries_.size() >= config_.capacity) {
        return std::unexpected(
            NavigationError{NavigationErrorCode::CapacityExceeded, entry.route_id, 0});
    }
    entries_.push_back(std::move(entry));
    touch();
    return PushOutcome::Committed;
}

std::expected<PushOutcome, NavigationError> NavController::push(std::string_view path) {
    auto entry = entry_for_path(path);
    if (!entry.has_value()) {
        return std::unexpected(entry.error());
    }
    return record(std::move(*entry), false);
}

std::expected<PushOutcome, NavigationError> NavController::push_route(std::string_view route_id,
                                                                      ParamMap params) {
    auto entry = entry_for_route(route_id, std::move(params));
    if (!entry.has_value()) {
        return std::unexpected(entry.error());
    }
    return record(std::move(*entry), false);
}

std::expected<PushOutcome, NavigationError> NavController::replace(std::string_view path) {
    auto entry = entry_for_path(path);
    if (!entry.has_value()) {
        return std::unexpected(entry.error());
    }
    return record(std::move(*entry), true);
}

std::expected<PushOutcome, NavigationError> NavController::replace_route(std::string_view route_id,
                                                                         ParamMap params) {
    auto entry = entry_for_route(route_id, std::move(params));
    if (!entry.has_value()) {
        return std::unexpected(entry.error());
    }
    return record(std::move(*entry), true);
}

BackResult NavController::back() {
    BackResult result;
    if (entries_.empty()) {
        // Section 2's empty-stack case: defined, reported, and it changes nothing. Reaching it
        // means the controller was built without an initial route and nothing has committed yet.
        result.outcome = BackOutcome::Empty;
        return result;
    }
    if (entries_.size() == 1) {
        // Section 2: "Back at root: exit/close per platform policy; never silently quit without
        // confirmation when unsaved work exists." The host owns that policy; the controller reports
        // the edge instead of closing a window or quitting, so the confirmation can be asked.
        result.outcome = BackOutcome::AtRoot;
        result.active_entry = entries_.back().entry_id;
        return result;
    }
    // Section 10.1 item 2 before item 3: the topmost modal/sheet entry is dismissed first, and the
    // host needs to know so it can restore focus.
    const bool modal = presentation_is_dismissible(entries_.back().presentation);
    entries_.pop_back();
    touch();
    result.outcome = modal ? BackOutcome::DismissedModal : BackOutcome::Popped;
    result.active_entry = entries_.back().entry_id;
    return result;
}

std::expected<void, NavigationError> NavController::pop_to(std::uint64_t entry_id) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].entry_id != entry_id) {
            continue;
        }
        if (index + 1 == entries_.size()) {
            return {}; // already active: a successful no-op, not an error
        }
        entries_.resize(index + 1);
        touch();
        return {};
    }
    return std::unexpected(NavigationError{NavigationErrorCode::UnknownEntry, {}, 0});
}

std::expected<void, NavigationError> NavController::replace_all(std::vector<NavEntry> entries,
                                                                std::size_t active_index) {
    if (entries.empty()) {
        return std::unexpected(NavigationError{NavigationErrorCode::EmptyStack, {}, 0});
    }
    if (entries.size() > config_.capacity) {
        return std::unexpected(NavigationError{NavigationErrorCode::CapacityExceeded, {}, 0});
    }
    if (active_index >= entries.size()) {
        return std::unexpected(NavigationError{NavigationErrorCode::UnknownEntry, {}, 0});
    }
    // Every entry is revalidated against the *current* table, so a restored stack and a scripted
    // one go through one check: section 10.1's "restore builds a stack of `NavEntry` with validated
    // route ids/params; invalid entries are dropped or replaced by the route root" is a property of
    // this method for every caller.
    for (NavEntry& entry : entries) {
        const RouteDef* route = table_->find(entry.route_id);
        if (route == nullptr) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnknownRouteId, entry.route_id, 0});
        }
        for (const ParamSpec& spec : route->params) {
            const std::string* value = find_param(entry.params, spec.name);
            if (value != nullptr && !validate_param_value(spec, *value)) {
                return std::unexpected(
                    NavigationError{NavigationErrorCode::ParamInvalid, spec.name, 0});
            }
        }
        if (entry.entry_id == 0) {
            entry.entry_id = next_entry_id();
        } else if (entry.entry_id > last_entry_id_) {
            // Section 10.1: ids are "stable for restore/automation", so a restored id is kept and
            // the allocator resumes above the highest one seen. A restored stack therefore cannot
            // reissue an id that an older entry still holds.
            last_entry_id_ = entry.entry_id;
        }
        if (route->presentation != Presentation::Page) {
            entry.presentation = route->presentation;
        }
    }
    // The stack's invariant is "back pops the end", so the active entry moves to the end. Section 2
    // has no forward stack, so entries above the snapshot's active one are dropped rather than kept
    // in an order back would pop wrongly.
    entries.resize(active_index + 1);
    entries_ = std::move(entries);
    touch();
    return {};
}

std::expected<void, NavigationError> NavController::reset_to(std::string_view path) {
    auto entry = entry_for_path(path);
    if (!entry.has_value()) {
        return std::unexpected(entry.error());
    }
    entries_.clear();
    entries_.push_back(std::move(*entry));
    touch();
    return {};
}

bool NavController::set_entry_state(std::uint64_t entry_id, StateMap state) {
    for (NavEntry& entry : entries_) {
        if (entry.entry_id == entry_id) {
            entry.state = std::move(state);
            touch();
            return true;
        }
    }
    return false;
}

const StateMap* NavController::entry_state(std::uint64_t entry_id) const noexcept {
    for (const NavEntry& entry : entries_) {
        if (entry.entry_id == entry_id) {
            return &entry.state;
        }
    }
    return nullptr;
}

bool NavController::commit() {
    if (!dirty_) {
        return false;
    }
    dirty_ = false;
    ++epoch_;
    // Two requests that ended on the same route (a push then a pop within one tick) publish one
    // value and wake nobody: AF4 compares, so the frame sees the route it already had.
    return route_.set(snapshot_of(entries_, epoch_));
}

} // namespace tessera::appfw
