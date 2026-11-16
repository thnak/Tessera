// SPDX-License-Identifier: MIT
//
// Fixtures for the navigation suite (issue #196). Everything here is explicit data: a route table
// with the four pattern forms, a `ManualIoExecutor` + in-memory file system so a restore never
// touches a disk, and an `UpdateScheduler` the test flushes by hand.
//
// Determinism: no wall clock, no OS, no network, no file, no random id. A deep link is a string and
// a restore is a buffer, which is what makes the four acceptance groups exact-equality assertions
// instead of "it did not crash".

#pragma once

#include "tessera/appfw/navigation_deep_link.hpp"
#include "tessera/appfw/navigation_restore.hpp"
#include "tessera/appfw/navigation_route.hpp"
#include "tessera/appfw/navigation_stack.hpp"
#include "tessera/appfw/storage.hpp"
#include "tessera/hal/filesystem.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/manual_io_executor.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nav_test {

using tessera::appfw::ActivationSource;
using tessera::appfw::BackOutcome;
using tessera::appfw::BackResult;
using tessera::appfw::DecodedSnapshot;
using tessera::appfw::DeepLink;
using tessera::appfw::DeepLinkConfig;
using tessera::appfw::DeepLinkOutcome;
using tessera::appfw::DeepLinkResult;
using tessera::appfw::DuplicatePolicy;
using tessera::appfw::NavController;
using tessera::appfw::NavControllerConfig;
using tessera::appfw::NavEntry;
using tessera::appfw::NavEntrySnapshot;
using tessera::appfw::NavigationError;
using tessera::appfw::NavigationErrorCode;
using tessera::appfw::NavRestoreStore;
using tessera::appfw::NavSnapshot;
using tessera::appfw::NavStackSnapshot;
using tessera::appfw::ParamMap;
using tessera::appfw::ParamSpec;
using tessera::appfw::ParamType;
using tessera::appfw::Presentation;
using tessera::appfw::PushOutcome;
using tessera::appfw::RestoreError;
using tessera::appfw::RestoreErrorCode;
using tessera::appfw::RestorePolicy;
using tessera::appfw::RouteDef;
using tessera::appfw::RouteMatch;
using tessera::appfw::RouteSnapshot;
using tessera::appfw::RouteTable;
using tessera::appfw::RouteTableConfig;
using tessera::appfw::StateMap;
using tessera::appfw::Storage;
using tessera::appfw::StorageConfig;
using tessera::hal::DirEntry;
using tessera::hal::Durability;
using tessera::hal::FileError;
using tessera::hal::FileErrorCode;
using tessera::hal::FileReadResult;
using tessera::hal::FileResult;
using tessera::hal::FileSystem;
using tessera::runtime::CancellationToken;
using tessera::runtime::ManualIoExecutor;
using tessera::ui::UpdateScheduler;

inline int g_failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_navigation: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// A fixture that cannot be built is a bug in this file, not an assertion about the module, so it
// ends the binary with a message instead of returning a value no assertion could interpret.
[[noreturn]] inline void fixture_failed(std::string_view what, std::string_view detail) {
    std::cerr << "appfw_navigation: fixture failed: " << what << "\n";
    if (!detail.empty()) {
        std::cerr << "  " << detail << "\n";
    }
    std::exit(1);
}

// --- the route table -----------------------------------------------------------------------------
//
// The spec's own example (navigation-v1.md section 1), plus the three things a matcher test needs
// and the example does not have: a literal that collides with a param route, an optional param, a
// wildcard, and a modal row (section 6). Deliberately small: every route below is referenced by at
// least one assertion, so an edit that orphans one shows up as a failing test rather than as dead
// data.
inline std::vector<RouteDef> spec_routes() {
    std::vector<RouteDef> routes;
    routes.push_back(RouteDef{"home", "/", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    routes.push_back(
        RouteDef{"orders", "/orders", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    RouteDef detail;
    detail.id = "orders.detail";
    detail.path = "/orders/:id";
    detail.params = {ParamSpec{"id", ParamType::Int, {}}};
    detail.auth = true;
    routes.push_back(detail);
    // A literal under the same prefix: `/orders/new` must win over `/orders/:id`, or the `Int`
    // validation would reject it and the screen would be unreachable.
    routes.push_back(RouteDef{
        "orders.new", "/orders/new", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    routes.push_back(
        RouteDef{"settings", "/settings", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    // The optional form: `/search` and `/search/books` both match.
    routes.push_back(RouteDef{
        "search", "/search/:scope?", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    // The wildcard form.
    routes.push_back(
        RouteDef{"files", "/files/*", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    // Section 6: a modal route is a stack entry with a presentation.
    routes.push_back(
        RouteDef{"compose", "/compose", {}, Presentation::Modal, false, DuplicatePolicy::Ignore});
    // Section 2's replace-on-duplicate policy, on its own row so the test names the policy and not
    // a global default.
    routes.push_back(
        RouteDef{"quick", "/quick", {}, Presentation::Page, false, DuplicatePolicy::Replace});
    // The not-found target (section 1's `notFound`).
    routes.push_back(RouteDef{
        "not_found", "/not-found", {}, Presentation::Page, false, DuplicatePolicy::Ignore});
    return routes;
}

inline RouteTableConfig spec_config() {
    RouteTableConfig config;
    config.routes = spec_routes();
    config.not_found_route = "not_found";
    config.initial_route = "home";
    config.max_path_segments = 32;
    config.max_params = 16;
    return config;
}

inline RouteTable make_table(RouteTableConfig config = spec_config()) {
    auto table = RouteTable::create(std::move(config));
    if (!table.has_value()) {
        fixture_failed("route table",
                       tessera::appfw::navigation_error_code_name(table.error().code));
    }
    return std::move(*table);
}

// --- the deterministic storage stack
// --------------------------------------------------------------

// The smallest conforming in-memory file system: enough for the atomic sequence, with one failure
// knob so the corruption path is induced rather than hoped for. It mirrors the AF7 suite's fake
// deliberately, so a reader who knows one knows the other.
class MemoryFileSystem final : public FileSystem {
public:
    enum class Fail { None, Read, Write, Replace };

    Fail fail = Fail::None;
    std::map<std::string, std::string> files;
    std::vector<std::string> steps;

    FileReadResult read(std::string_view path, std::size_t max_bytes) override {
        steps.emplace_back("read");
        if (fail == Fail::Read) {
            return std::unexpected(FileError{FileErrorCode::Io, "read", 0});
        }
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "read", 0});
        }
        if (found->second.size() > max_bytes) {
            return std::unexpected(FileError{FileErrorCode::TooLarge, "read", 0});
        }
        return found->second;
    }

    FileResult write(std::string_view path, std::string_view data) override {
        steps.emplace_back("write");
        if (fail == Fail::Write) {
            return std::unexpected(FileError{FileErrorCode::Io, "write", 0});
        }
        files[std::string(path)] = std::string(data);
        return {};
    }

    FileResult flush(std::string_view path) override {
        steps.emplace_back("flush");
        return files.find(std::string(path)) == files.end()
                   ? FileResult{std::unexpected(FileError{FileErrorCode::NotFound, "flush", 0})}
                   : FileResult{};
    }

    FileResult flush_directory(std::string_view) override {
        steps.emplace_back("flush-directory");
        return {};
    }

    FileResult replace(std::string_view from, std::string_view to) override {
        steps.emplace_back("replace");
        if (fail == Fail::Replace) {
            return std::unexpected(FileError{FileErrorCode::Io, "replace", 0});
        }
        const auto source = files.find(std::string(from));
        if (source == files.end()) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "replace", 0});
        }
        files[std::string(to)] = source->second;
        files.erase(source);
        return {};
    }

    FileResult remove(std::string_view path) override {
        steps.emplace_back("remove");
        return files.erase(std::string(path)) == 0
                   ? FileResult{std::unexpected(FileError{FileErrorCode::NotFound, "remove", 0})}
                   : FileResult{};
    }

    FileResult create_directory(std::string_view) override {
        steps.emplace_back("create-directory");
        return {};
    }

    bool exists(std::string_view path) override {
        return files.find(std::string(path)) != files.end();
    }

    std::expected<std::uint64_t, FileError> size(std::string_view path) override {
        const auto found = files.find(std::string(path));
        return found == files.end() ? std::expected<std::uint64_t, FileError>{std::unexpected(
                                          FileError{FileErrorCode::NotFound, "size", 0})}
                                    : std::expected<std::uint64_t, FileError>{
                                          static_cast<std::uint64_t>(found->second.size())};
    }

    std::expected<std::vector<DirEntry>, FileError> list(std::string_view) override {
        return std::vector<DirEntry>{};
    }

    [[nodiscard]] std::string content_of(std::string_view path) const {
        const auto found = files.find(std::string(path));
        return found == files.end() ? std::string() : found->second;
    }
};

// The whole deterministic stack: a manual executor, the in-memory file system and a `Storage`.
// Tests drive it with `run()`; `run_one()` = one I/O body plus its completion, which is one whole
// storage operation.
struct StorageHarness {
    StorageHarness()
        : config{"/nav", 256 * 1024, Durability::BestEffort, 1, 4},
          store(config, executor, files) {}

    ManualIoExecutor executor;
    MemoryFileSystem files;
    StorageConfig config;
    Storage store;

    void run_one() {
        executor.run_one();
        executor.run_completions(1);
    }
    void run_all() {
        executor.run(64);
        executor.run_completions(64);
    }
};

// --- the controller
// -------------------------------------------------------------------------------

// The scheduler must outlive the controller that attaches a ticket to it, so it is a member of the
// harness declared before it, and this type is not movable.
struct Harness {
    explicit Harness(RouteTableConfig config = spec_config(),
                     NavControllerConfig controller_config = {})
        : table(make_table(std::move(config))),
          controller(table, scheduler, controller_config) {}

    UpdateScheduler scheduler;
    RouteTable table;
    NavController controller;
};

// Records what a subscriber of the route observable saw. The callback takes `const RouteSnapshot&`
// because that is `Observable<T>`'s contract: the value is live for the duration of the call, never
// a copy.
struct RouteRecorder {
    static void on_route(void* context, const RouteSnapshot& snapshot) {
        auto* self = static_cast<RouteRecorder*>(context);
        ++self->calls;
        self->last_route_id = snapshot.entry.route_id;
        self->last_epoch = snapshot.epoch;
        self->last_depth = snapshot.depth;
        self->last_at_root = snapshot.at_root;
    }

    int calls = 0;
    std::string last_route_id;
    std::uint64_t last_epoch = 0;
    std::size_t last_depth = 0;
    bool last_at_root = false;
};

// The deep-link declaration the tests use: one custom scheme, one HTTPS host (deep-linking-v1.md
// section 3's two allowlists).
inline DeepLinkConfig link_config(bool accept_bare_path = false) {
    DeepLinkConfig config;
    config.schemes = {"myapp", "https"};
    config.hosts = {"app.example.com"};
    config.accept_bare_path = accept_bare_path;
    return config;
}

} // namespace nav_test
