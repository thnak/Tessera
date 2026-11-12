// SPDX-License-Identifier: MIT

#include "script_internal.hpp"
#include "tessera/harness/command_result.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/script_runner.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::harness {
namespace {

std::int64_t as_json_integer(std::uint64_t value) noexcept {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::min(value, kMax));
}

std::uint64_t add_saturating(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

JsonValue encode_error(const ScriptError& error) {
    JsonValue::Object members;
    members.emplace_back("code", JsonValue::string(std::string(to_string(error.code))));
    if (!error.message.empty()) {
        members.emplace_back("message", JsonValue::string(error.message));
    }
    if (!error.detail.empty()) {
        members.emplace_back("detail", JsonValue::string(error.detail));
    }
    if (error.step != kNoScriptStep) {
        members.emplace_back("step", JsonValue::integer(as_json_integer(error.step)));
    }
    if (error.protocol_code.has_value()) {
        members.emplace_back("protocolCode",
                             JsonValue::string(std::string(to_string(*error.protocol_code))));
    }
    return JsonValue::object(std::move(members));
}

ScriptError make_step_error(ScriptErrorCode code, std::string message, std::string detail,
                            std::size_t index,
                            std::optional<ProtocolErrorCode> protocol_code = std::nullopt) {
    ScriptError error;
    error.code = code;
    error.message = std::move(message);
    error.detail = std::move(detail);
    error.step = index;
    error.protocol_code = protocol_code;
    return error;
}

} // namespace

ScriptRunner::ScriptRunner(Script script, CommandHandler& handler, ArtifactSink& sink,
                           SessionClock& clock, ScriptBudget budget)
    : script_(std::move(script)),
      handler_(handler),
      sink_(sink),
      clock_(clock),
      budget_(budget) {
    start_us_ = clock_.now_us();
    report_.steps_total = script_.steps.size();
    if (budget_.run_timeout_us != 0) {
        run_deadline_us_ = add_saturating(start_us_, budget_.run_timeout_us);
    }
}

std::uint64_t ScriptRunner::elapsed_since(std::uint64_t start_us) const noexcept {
    const std::uint64_t now = clock_.now_us();
    return now >= start_us ? now - start_us : 0;
}

void ScriptRunner::finalize() noexcept {
    finished_ = true;
    report_.elapsed_us = elapsed_since(start_us_);
    report_.steps_executed = report_.steps.size();
    report_.cancelled = cancelled_;
}

void ScriptRunner::fail_run(ScriptErrorCode code, std::string message, std::string detail) {
    report_.passed = false;
    report_.error =
        ScriptError{code, std::move(message), std::move(detail), kNoScriptStep, std::nullopt};
    finalize();
}

void ScriptRunner::record_step(ScriptStepReport step_report) {
    if (!step_report.ok && report_.passed) {
        report_.passed = false;
    }
    report_.steps.push_back(std::move(step_report));
}

bool ScriptRunner::execute(const ScriptStep& step, std::uint64_t start_us,
                           std::uint64_t step_budget_us) {
    const std::size_t index = next_ - 1;
    CommandPayload command;
    switch (step.action) {
    case ScriptAction::Click:
    case ScriptAction::Type:
    case ScriptAction::Focus:
    case ScriptAction::Scroll:
    case ScriptAction::ScrollIntoView: {
        ActionCommand action;
        action.kind = step.action == ScriptAction::Click    ? ActionKind::Click
                      : step.action == ScriptAction::Type   ? ActionKind::SetValue
                      : step.action == ScriptAction::Focus  ? ActionKind::Focus
                      : step.action == ScriptAction::Scroll ? ActionKind::Scroll
                                                            : ActionKind::ScrollIntoView;
        action.ui_id = step.ui_id.value_or(0);
        action.item_key = step.item_key;
        action.item_index = step.item_index;
        action.alignment = step.alignment;
        action.value = step.text;
        action.scroll_dx = step.scroll_dx;
        action.scroll_dy = step.scroll_dy;
        command = std::move(action);
        break;
    }
    case ScriptAction::Snapshot:
        command = SnapshotCommand{step.include_hidden};
        break;
    case ScriptAction::Trace:
        command = TraceCommand{step.path};
        break;
    case ScriptAction::Audit:
        command = AuditCommand{step.profile, step.path};
        break;
    case ScriptAction::Assert:
    case ScriptAction::WaitFor:
        // Hidden nodes stay in the document so `visible:false` and `exists:false` differ.
        command = SnapshotCommand{true};
        break;
    }

    ScriptStepReport step_report;
    step_report.index = index;
    step_report.action = step.action;
    step_report.ui_id = step.ui_id;
    step_report.item_key = step.item_key;

    auto outcome = handler_.handle(command);
    step_report.elapsed_us = elapsed_since(start_us);

    if (step_report.elapsed_us > step_budget_us) {
        // Same rule as SessionExecutor: a synchronous handler cannot be preempted, so an overrun
        // is reported after the fact and the run stops instead of pretending the step succeeded.
        step_report.error = make_step_error(
            ScriptErrorCode::StepTimeout, "step exceeded its time budget",
            std::to_string(step_report.elapsed_us) + " > " + std::to_string(step_budget_us) + " us",
            index, ProtocolErrorCode::Timeout);
        record_step(std::move(step_report));
        return false;
    }
    if (!outcome.has_value()) {
        const ProtocolError& error = outcome.error();
        step_report.error = make_step_error(ScriptErrorCode::HandlerError, error.message,
                                            error.detail, index, error.code);
        record_step(std::move(step_report));
        return false;
    }

    auto encoded = encode_command_result(*outcome);
    if (!encoded.has_value()) {
        step_report.error = make_step_error(ScriptErrorCode::Internal, encoded.error().message,
                                            encoded.error().detail, index, encoded.error().code);
        record_step(std::move(step_report));
        return false;
    }
    step_report.result = std::move(*encoded);

    if (step.action == ScriptAction::Assert || step.action == ScriptAction::WaitFor) {
        const auto& snapshot = std::get<SnapshotResult>(*outcome);
        if (auto failure = detail::evaluate_assertion(step, snapshot.snapshot);
            failure.has_value()) {
            // Only the verdict is reported: embedding the whole snapshot per assertion would bloat
            // the report without helping the reader.
            step_report.result = JsonValue::object({});
            step_report.error = make_step_error(ScriptErrorCode::AssertionFailed,
                                                "assertion failed", std::move(*failure), index);
            record_step(std::move(step_report));
            return false;
        }
        step_report.result = JsonValue::object({});
        step_report.ok = true;
        record_step(std::move(step_report));
        return true;
    }

    if (step.action == ScriptAction::Snapshot) {
        const auto& snapshot = std::get<SnapshotResult>(*outcome);
        auto document = to_json(snapshot.snapshot);
        if (!document.has_value()) {
            step_report.error =
                make_step_error(ScriptErrorCode::Internal, "snapshot document could not be encoded",
                                "snapshot", index);
            record_step(std::move(step_report));
            return false;
        }
        if (document->size() > kMaxArtifactBytes) {
            step_report.error = make_step_error(ScriptErrorCode::ArtifactError,
                                                "snapshot document exceeds the artifact cap",
                                                step.path.empty() ? "snapshot" : step.path, index);
            record_step(std::move(step_report));
            return false;
        }
        if (!step.path.empty()) {
            auto written = sink_.write(step.path, *document);
            if (!written.has_value()) {
                step_report.error =
                    make_step_error(ScriptErrorCode::ArtifactError, written.error().message,
                                    written.error().detail, index, written.error().code);
                record_step(std::move(step_report));
                return false;
            }
            step_report.artifact_path = step.path;
        }
    } else if (step.action == ScriptAction::Trace) {
        step_report.artifact_path = std::get<TraceResult>(*outcome).path;
    } else if (step.action == ScriptAction::Audit) {
        step_report.artifact_path = std::get<AuditResult>(*outcome).path;
    }

    step_report.ok = true;
    record_step(std::move(step_report));
    return true;
}

bool ScriptRunner::pump() {
    if (finished_) {
        return false;
    }
    if (cancelled_) {
        fail_run(ScriptErrorCode::Cancelled, "script run was cancelled", {});
        return false;
    }
    if (next_ >= script_.steps.size()) {
        report_.passed = true;
        finalize();
        return false;
    }

    const std::uint64_t now = clock_.now_us();
    if (next_ >= budget_.max_steps) {
        fail_run(ScriptErrorCode::StepCapExceeded, "script step cap reached",
                 std::to_string(script_.steps.size()) + " steps, cap " +
                     std::to_string(budget_.max_steps));
        return false;
    }
    if (run_deadline_us_ != 0 && now >= run_deadline_us_) {
        fail_run(ScriptErrorCode::RunTimeout, "script run exceeded its time budget", {});
        return false;
    }

    const ScriptStep& step = script_.steps[next_];
    std::uint64_t step_budget_us = budget_.step_timeout_us;
    if (step.timeout_us != 0 && step.timeout_us < step_budget_us) {
        step_budget_us = step.timeout_us;
    }
    if (run_deadline_us_ != 0) {
        const std::uint64_t remaining = run_deadline_us_ > now ? run_deadline_us_ - now : 0;
        step_budget_us = std::min(step_budget_us, remaining);
    }
    ++next_;
    if (now >= add_saturating(now, step_budget_us)) {
        ScriptStepReport step_report;
        step_report.index = next_ - 1;
        step_report.action = step.action;
        step_report.ui_id = step.ui_id;
        step_report.item_key = step.item_key;
        step_report.error =
            make_step_error(ScriptErrorCode::StepTimeout, "step deadline expired before dispatch",
                            "stepBudget", step_report.index, ProtocolErrorCode::Timeout);
        record_step(std::move(step_report));
        finalize();
        return false;
    }

    if (!execute(step, now, step_budget_us)) {
        finalize();
        return false;
    }
    if (next_ >= script_.steps.size()) {
        report_.passed = true;
        finalize();
        return false;
    }
    return true;
}

void ScriptRunner::cancel() noexcept {
    if (!finished_) {
        cancelled_ = true;
    }
}

JsonValue encode_script_report(const ScriptReport& report) {
    JsonValue::Object members;
    members.emplace_back("scriptVersion", JsonValue::integer(1));
    members.emplace_back("passed", JsonValue::boolean(report.passed));
    members.emplace_back("cancelled", JsonValue::boolean(report.cancelled));
    members.emplace_back("elapsedUs", JsonValue::integer(as_json_integer(report.elapsed_us)));
    members.emplace_back("stepsRun", JsonValue::integer(as_json_integer(report.steps_executed)));
    members.emplace_back("stepsTotal", JsonValue::integer(as_json_integer(report.steps_total)));

    JsonValue::Array steps;
    steps.reserve(report.steps.size());
    for (const ScriptStepReport& step : report.steps) {
        JsonValue::Object members_step;
        members_step.emplace_back("index", JsonValue::integer(as_json_integer(step.index)));
        members_step.emplace_back("action", JsonValue::string(std::string(to_string(step.action))));
        if (step.ui_id.has_value()) {
            members_step.emplace_back("uiId", JsonValue::string(std::to_string(*step.ui_id)));
        }
        if (step.item_key.has_value()) {
            members_step.emplace_back("itemKey", JsonValue::string(*step.item_key));
        }
        members_step.emplace_back("ok", JsonValue::boolean(step.ok));
        members_step.emplace_back("elapsedUs",
                                  JsonValue::integer(as_json_integer(step.elapsed_us)));
        if (step.ok) {
            members_step.emplace_back("result", step.result);
        } else {
            members_step.emplace_back("error", encode_error(step.error));
        }
        if (!step.artifact_path.empty()) {
            members_step.emplace_back("artifact", JsonValue::string(step.artifact_path));
        }
        steps.push_back(JsonValue::object(std::move(members_step)));
    }
    members.emplace_back("steps", JsonValue::array(std::move(steps)));
    if (report.error.has_value()) {
        members.emplace_back("error", encode_error(*report.error));
    }
    return JsonValue::object(std::move(members));
}

} // namespace tessera::harness
