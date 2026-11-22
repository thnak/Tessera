// SPDX-License-Identifier: MIT

#include "tessera/appfw/permission_service.hpp"

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tessera::appfw {
namespace {

std::size_t permission_index(Permission permission) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint8_t>(permission));
}

PermissionResult policy_answer(Permission permission, PermissionState state) {
    PermissionResult result;
    result.permission = permission;
    result.state = state;
    return result;
}

PermissionResult error_answer(Permission permission, PermissionErrorKind kind,
                              std::string detail = {}) {
    PermissionResult result;
    result.permission = permission;
    result.state = PermissionState::Unknown;
    result.error = PermissionError{kind, std::move(detail)};
    result.has_error = true;
    return result;
}

} // namespace

// The impl is one object shared with in-flight completions, so a destroyed service cannot deliver
// to a freed callback: the service drops its `shared_ptr` and a late completion finds the impl
// alive but detached.
//
// It lives in `detail`, which is where the header's forward declaration names it. Defining it in
// `appfw` directly would give the compiler two unrelated types and every member access below would
// fail with "use of undefined type".
namespace detail {

class PermissionServiceImpl : public std::enable_shared_from_this<PermissionServiceImpl> {
public:
    PermissionServiceImpl(PermissionConfig config, PermissionBackend& backend,
                          runtime::CompletionQueue& completions)
        : config_(config),
          backend_(&backend),
          completions_(&completions),
          capabilities_(backend.capabilities()) {
        states_.fill(PermissionState::NotDetermined);
        // Seed the cache from the platform, without prompting: a permission the user already
        // granted must not look `NotDetermined` on the first `status()`.
        for (std::size_t index = 0; index < kPermissionCount; ++index) {
            const auto permission = static_cast<Permission>(index);
            states_[index] = capabilities_.supports(permission) ? backend.status(permission)
                                                                : PermissionState::NotDetermined;
        }
    }

    PermissionServiceImpl(const PermissionServiceImpl&) = delete;
    PermissionServiceImpl& operator=(const PermissionServiceImpl&) = delete;

    [[nodiscard]] PermissionState status(Permission permission) const noexcept {
        return states_[permission_index(permission)];
    }

    std::size_t refresh() {
        capabilities_ = backend_->capabilities();
        std::size_t changed = 0;
        for (std::size_t index = 0; index < kPermissionCount; ++index) {
            const auto permission = static_cast<Permission>(index);
            if (!capabilities_.supports(permission)) {
                continue;
            }
            const PermissionState next = backend_->status(permission);
            if (states_[index] != next) {
                states_[index] = next;
                ++changed;
            }
        }
        return changed;
    }

    [[nodiscard]] const PermissionBackendCapabilities& capabilities() const noexcept {
        return capabilities_;
    }

    [[nodiscard]] bool declared(Permission permission) const noexcept {
        return (config_.declared & permission_mask(permission)) != 0;
    }

    PermissionTicket submit(Permission permission, PermissionRequestOptions options,
                            runtime::CancellationToken token, PermissionService::Callback on_done) {
        ++stats_.requests;

        // --- the four "do not ask" rules (spec section 5 rules 2, 3, 4 and 7)
        // ---------------------
        //
        // Answered on the next completion drain rather than on this stack, so a caller cannot
        // observe a difference between "answered by policy" and "answered by the OS". That is what
        // makes `submit` usable in the same place for both.
        if (const std::optional<PermissionResult> immediate = policy_result(permission, options);
            immediate.has_value()) {
            return settle_immediately(permission, *immediate, std::move(on_done));
        }

        // --- the declaration check (spec section 7 rule 2) --------------------------------------
        if (!declared(permission)) {
            return settle_immediately(permission,
                                      error_answer(permission, PermissionErrorKind::NotDeclared,
                                                   std::string(permission_name(permission))),
                                      std::move(on_done));
        }

        // --- coalescing (spec section 5 rule 5) -------------------------------------------------
        //
        // A second request for a permission already pending joins the first: one platform call, one
        // prompt, one answer for every caller. This is the issue's acceptance criterion, and it is
        // why the table is keyed by permission rather than by ticket.
        if (PendingRequest* existing = pending_for(permission); existing != nullptr) {
            ++stats_.coalesced;
            existing->callbacks.push_back(std::move(on_done));
            return existing->ticket;
        }

        if (pending_.size() >= kMaxPendingPermissionRequests) {
            // The bound is reached. Refusing with a typed error is better than growing the table
            // without limit, and `InvalidState` names whose problem it is.
            return settle_immediately(permission,
                                      error_answer(permission, PermissionErrorKind::InvalidState,
                                                   "too many pending permission requests"),
                                      std::move(on_done));
        }

        // --- the platform (spec section 5 rule 1: never blocks) ---------------------------------
        const PermissionTicket ticket{next_ticket_++};
        PendingRequest pending;
        pending.ticket = ticket;
        pending.permission = permission;
        pending.callbacks.push_back(std::move(on_done));
        pending_.push_back(std::move(pending));

        const std::weak_ptr<PermissionServiceImpl> weak = weak_from_this();
        const bool accepted = backend_->request(
            permission, std::move(options), token,
            [weak, permission, ticket](std::expected<PermissionState, PermissionError> answer) {
                if (const std::shared_ptr<PermissionServiceImpl> self = weak.lock()) {
                    self->complete(ticket, permission, std::move(answer));
                }
                // A service destroyed before the answer arrives drops it here: spec section 5
                // rule 9.
            });
        if (!accepted) {
            // The back end cannot serve it at all. Take the callback back out of the pending entry
            // *before* removing it, so the caller still hears an answer: a refused request that
            // never completes is a hang in the app's code, which is the one thing worse than a
            // typed error. Removing the entry first also means a retry is not coalesced onto a
            // request that will never finish.
            std::vector<PermissionService::Callback> callbacks;
            for (PendingRequest& candidate : pending_) {
                if (candidate.ticket == ticket) {
                    callbacks = std::move(candidate.callbacks);
                    break;
                }
            }
            remove_pending(permission);
            const PermissionResult refused =
                error_answer(permission, PermissionErrorKind::Unsupported);
            ++stats_.unsupported;
            store(ticket, refused, permission);
            if (callbacks.empty()) {
                post(ticket, refused, {});
            }
            for (PermissionService::Callback& callback : callbacks) {
                post(ticket, refused, std::move(callback));
            }
            return ticket;
        }
        return ticket;
    }

    std::optional<PermissionResult> result_of(PermissionTicket ticket) const {
        for (const Record& record : records_) {
            if (record.ticket == ticket && record.finished) {
                return record.result;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const PermissionService::Stats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept {
        return backend_->name();
    }

    // The completion queue, so the coroutine wrapper can build its awaiter against the same queue
    // the service posts to. Public on the impl and not on the service: an app has no business
    // naming it.
    [[nodiscard]] runtime::CompletionQueue& completions() const noexcept {
        return *completions_;
    }

private:
    struct PendingRequest {
        PermissionTicket ticket{};
        Permission permission = Permission::Camera;
        std::vector<PermissionService::Callback> callbacks;
    };

    struct Record {
        PermissionTicket ticket{};
        PermissionResult result{};
        bool finished = false;
    };

    // The state rules of section 5, as a pure function of the cached table. Returns a result when
    // the module should answer without calling the platform, nullopt when the platform must be
    // asked.
    [[nodiscard]] std::optional<PermissionResult>
    policy_result(Permission permission, const PermissionRequestOptions& options) const {
        if (!capabilities_.supports(permission)) {
            return error_answer(permission, PermissionErrorKind::Unsupported);
        }
        // Section 3's `none` column and the desktop rows: granted by presence, no dialogue exists
        // to show. Reporting `Granted` is the honest answer (section 9 item 4 forbids faking a
        // prompt).
        if (capabilities_.is_promptless(permission)) {
            return policy_answer(permission, PermissionState::Granted);
        }
        const PermissionState state = status(permission);
        // Rule 2: asking for what you already have does not prompt.
        if (state == PermissionState::Granted) {
            return policy_answer(permission, PermissionState::Granted);
        }
        // Rule 3: a permanently denied permission never prompts again.
        if (state == PermissionState::PermanentlyDenied) {
            return policy_answer(permission, PermissionState::PermanentlyDenied);
        }
        // Rule 4: a policy refusal cannot be retried.
        if (state == PermissionState::Restricted) {
            return policy_answer(permission, PermissionState::Restricted);
        }
        // Rule 7: a denial the app has opted out of retrying does not prompt.
        if (state == PermissionState::Denied && !options.retry_after_denial) {
            return policy_answer(permission, PermissionState::Denied);
        }
        return std::nullopt;
    }

    PermissionTicket settle_immediately(Permission permission, PermissionResult result,
                                        PermissionService::Callback on_done) {
        const PermissionTicket ticket{next_ticket_++};
        if (!result.has_error) {
            ++stats_.answered_without_prompt;
            record_outcome(result.state);
        } else if (result.error.kind == PermissionErrorKind::NotDeclared) {
            ++stats_.undeclared;
        } else if (result.error.kind == PermissionErrorKind::Unsupported) {
            ++stats_.unsupported;
        }
        // The state table moves only for a state answer, never for an error (section 5 rule 8).
        store(ticket, result, permission);
        post(ticket, std::move(result), std::move(on_done));
        return ticket;
    }

    void complete(PermissionTicket ticket, Permission permission,
                  std::expected<PermissionState, PermissionError> answer) {
        PendingRequest* entry = nullptr;
        for (PendingRequest& candidate : pending_) {
            if (candidate.ticket == ticket) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            // The request was cancelled or its record replaced: the completion is stale.
            ++stats_.stale_completions;
            return;
        }
        std::vector<PermissionService::Callback> callbacks = std::move(entry->callbacks);
        remove_pending(permission);

        PermissionResult result;
        result.permission = permission;
        if (answer.has_value()) {
            result.state = *answer;
            // Section 5 rule 8: the table moves only on a state answer.
            states_[permission_index(permission)] = *answer;
            record_outcome(*answer);
        } else {
            result.state = PermissionState::Unknown;
            result.error = std::move(answer.error());
            result.has_error = true;
        }
        store(ticket, result, permission);

        // Every coalesced caller gets the one answer, in the order it joined.
        for (PermissionService::Callback& callback : callbacks) {
            post(ticket, result, std::move(callback));
        }
    }

    void record_outcome(PermissionState state) noexcept {
        switch (state) {
        case PermissionState::Granted:
            ++stats_.granted;
            break;
        case PermissionState::Denied:
            ++stats_.denied;
            break;
        case PermissionState::PermanentlyDenied:
            ++stats_.permanently_denied;
            break;
        case PermissionState::Restricted:
            ++stats_.restricted;
            break;
        case PermissionState::NotDetermined:
        case PermissionState::Unknown:
            break;
        }
    }

    // Posts the callback, never calls it: a caller may not observe an answer on the `submit` stack
    // (seam contract 2, applied to the module's own answers too).
    void post(PermissionTicket ticket, PermissionResult result,
              PermissionService::Callback on_done) {
        if (!on_done) {
            return;
        }
        (void)completions_->post(runtime::CompletionPriority::Visible,
                                 [on_done = std::move(on_done),
                                  result = std::move(result)]() mutable { on_done(result); });
        (void)ticket;
    }

    // Keeps one terminal record per ticket so `result_of` answers after completion. The oldest
    // terminal record is evicted first; a pending request is never evicted.
    void store(PermissionTicket ticket, const PermissionResult& result, Permission permission) {
        (void)permission;
        records_.push_back(Record{ticket, result, true});
        while (records_.size() > config_.max_records) {
            records_.erase(records_.begin());
        }
    }

    [[nodiscard]] PendingRequest* pending_for(Permission permission) noexcept {
        for (PendingRequest& entry : pending_) {
            if (entry.permission == permission) {
                return &entry;
            }
        }
        return nullptr;
    }

    void remove_pending(Permission permission) {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->permission == permission) {
                pending_.erase(it);
                return;
            }
        }
    }

    PermissionConfig config_;
    PermissionBackend* backend_ = nullptr;
    runtime::CompletionQueue* completions_ = nullptr;
    PermissionBackendCapabilities capabilities_;
    std::array<PermissionState, kPermissionCount> states_{};
    std::vector<PendingRequest> pending_;
    std::vector<Record> records_;
    PermissionService::Stats stats_;
    std::uint64_t next_ticket_ = 1;
};

} // namespace detail

// --- PermissionService
// -----------------------------------------------------------------------------
//
// The public class holds the impl by a `shared_ptr` while the impl's in-flight completions hold it
// by `weak_ptr`. Destroying the service therefore detaches it: a prompt that answers afterwards
// finds no live impl and drops the answer (spec section 5 rule 9) instead of calling into freed
// state. A `unique_ptr` here would make `weak_from_this()` always empty and drop every answer
// instead.

PermissionService::PermissionService(PermissionConfig config, PermissionBackend& backend,
                                     runtime::CompletionQueue& completions)
    // `make_shared`, not `make_unique`: the impl hands a `weak_ptr` to every in-flight prompt so a
    // destroyed service drops a late answer instead of delivering it into freed state. With
    // `make_unique` there is no shared control block, `weak_from_this()` is always empty, and
    // *every* platform answer is silently dropped - which reads as "permissions never resolve"
    // rather than as a lifetime error. The header stores this as a `shared_ptr` for the same
    // reason.
    : impl_(std::make_shared<detail::PermissionServiceImpl>(config, backend, completions)) {}

PermissionService::~PermissionService() = default;

PermissionState PermissionService::status(Permission permission) const noexcept {
    return impl_->status(permission);
}

std::size_t PermissionService::refresh() {
    return impl_->refresh();
}

const PermissionBackendCapabilities& PermissionService::capabilities() const noexcept {
    return impl_->capabilities();
}

bool PermissionService::declared(Permission permission) const noexcept {
    return impl_->declared(permission);
}

PermissionTicket PermissionService::submit(Permission permission, PermissionRequestOptions options,
                                           runtime::CancellationToken token, Callback on_done) {
    return impl_->submit(permission, std::move(options), token, std::move(on_done));
}

runtime::Task<PermissionResult> PermissionService::request(Permission permission,
                                                           PermissionRequestOptions options,
                                                           runtime::CancellationToken token) {
    return request_body(runtime::TaskContext{}, *this, permission, std::move(options),
                        std::move(token));
}

std::optional<PermissionResult> PermissionService::result_of(PermissionTicket ticket) const {
    return impl_->result_of(ticket);
}

const PermissionService::Stats& PermissionService::stats() const noexcept {
    return impl_->stats();
}

std::string_view PermissionService::backend_name() const noexcept {
    return impl_->backend_name();
}

runtime::CompletionQueue& PermissionService::completion_queue() const noexcept {
    return impl_->completions();
}

} // namespace tessera::appfw
