// SPDX-License-Identifier: MIT

#include "scene_pending.hpp"
#include "scene_session.hpp"
#include "tessera/ui/pending/resource_view.hpp"

#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {

bool pending_action_from(std::string_view name, PendingAction& out) noexcept {
    if (name == "fetch") {
        out = PendingAction::Fetch;
        return true;
    }
    if (name == "resolve") {
        out = PendingAction::Resolve;
        return true;
    }
    if (name == "fail") {
        out = PendingAction::Fail;
        return true;
    }
    return false;
}

PendingPreview::PendingPreview(const ir::Component& component)
    : component_(component),
      texts_(component.resources.size()),
      errors_(component.resources.size()) {
    session_.configure(component);
    for (std::size_t index = 0; index < component.resources.size(); ++index) {
        tessera::ui::pending::FakeResource& fake = doubles_.emplace_back();
        (void)fake.begin_fetch();
        session_.bind_resource(index, tessera::ui::pending::ResourceView::of(fake));
    }
    session_.sync();
}

std::optional<std::size_t> PendingPreview::find(std::string_view resource) const noexcept {
    for (std::size_t index = 0; index < component_.resources.size(); ++index) {
        const std::string_view id = component_.resources[index].id;
        if (id == resource || (id.starts_with("res::") && id.substr(5) == resource)) {
            return index;
        }
    }
    return std::nullopt;
}

PendingControlResult PendingPreview::control(const PendingControl& control) {
    PendingControlResult result;
    const std::optional<std::size_t> found = find(control.resource);
    if (!found) {
        result.error = "no resource `" + control.resource + "` in this component";
        return result;
    }
    const std::size_t index = *found;
    tessera::ui::pending::FakeResource& fake = doubles_[index];
    result.ok = true;
    if (control.action == PendingAction::Fetch) {
        (void)fake.begin_fetch();
        result.accepted = true;
        session_.sync();
        return result;
    }
    const std::uint64_t epoch = control.epoch != 0 ? control.epoch : fake.epoch();
    // The double drops a stale or unsolicited completion itself; checking first keeps the bytes it
    // is still publishing alive when that happens.
    if (!fake.loading() || epoch != fake.epoch()) {
        session_.sync();
        return result;
    }
    if (control.action == PendingAction::Fail) {
        errors_[index] = control.message;
        result.accepted = fake.fail(epoch, errors_[index]);
    } else if (component_.resources[index].type == "string") {
        texts_[index] = control.text;
        result.accepted = fake.resolve_text(epoch, texts_[index]);
    } else {
        result.accepted = fake.resolve_number(epoch, control.number);
    }
    session_.sync();
    return result;
}

void PendingPreview::after_handler() noexcept {
    const std::size_t watched = session_.watched();
    if (watched >= doubles_.size()) {
        return;
    }
    const tessera::ui::pending::TransitionMark& mark = session_.transition();
    if (mark.pending() && mark.epoch() == doubles_[watched].epoch() + 1) {
        (void)doubles_[watched].begin_fetch();
    }
}

PendingReport PendingPreview::report() const {
    PendingReport out;
    out.active = !component_.resources.empty();
    out.is_pending = session_.is_pending();
    for (std::size_t index = 0; index < component_.resources.size(); ++index) {
        const tessera::ui::pending::ResourceState state = session_.resource_state(index);
        out.resources.push_back({component_.resources[index].id, state.status(), state.epoch,
                                 std::string(state.error)});
    }
    for (std::size_t index = 0; index < session_.boundary_count(); ++index) {
        const ir::Node* node = session_.boundary_node(index);
        const tessera::ui::pending::SuspenseBoundary& boundary = session_.boundary_at(index);
        out.boundaries.push_back({node != nullptr ? node->id : std::string(),
                                  boundary.fallback_live(), boundary.held()});
    }
    return out;
}

PendingControlResult PreviewSession::control_resource(const PendingControl& control) {
    if (!pending_) {
        PendingControlResult none;
        none.error = "this component declares no `useResource`";
        return none;
    }
    PendingControlResult result = pending_->control(control);
    if (result.ok && result.accepted) {
        // The tree is derived from the model, so a change of the model is a rebuild: the same step
        // a handler that changed state takes (`dispatch`).
        rebuild();
    }
    return result;
}

PendingReport PreviewSession::pending_report() const {
    return pending_ ? pending_->report() : PendingReport{};
}

namespace {

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            out.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void append_flag(std::string& out, std::string_view name, bool value) {
    out += ",\"";
    out += name;
    out += value ? "\":true" : "\":false";
}

} // namespace

std::string pending_report_json(const PendingControlResult& result, const PendingReport& report) {
    std::string out = "{\"pendingVersion\":1";
    append_flag(out, "ok", result.ok);
    append_flag(out, "accepted", result.accepted);
    append_flag(out, "changed", result.accepted);
    append_flag(out, "isPending", report.is_pending);
    out += ",\"resources\":[";
    for (std::size_t index = 0; index < report.resources.size(); ++index) {
        const PendingResourceReport& resource = report.resources[index];
        out += index == 0 ? "{\"id\":" : ",{\"id\":";
        append_quoted(out, resource.id);
        out += ",\"status\":";
        append_quoted(out, resource.status);
        out += ",\"epoch\":" + std::to_string(resource.epoch) + ",\"error\":";
        append_quoted(out, resource.error);
        out.push_back('}');
    }
    out += "],\"boundaries\":[";
    for (std::size_t index = 0; index < report.boundaries.size(); ++index) {
        const PendingBoundaryReport& boundary = report.boundaries[index];
        out += index == 0 ? "{\"id\":" : ",{\"id\":";
        append_quoted(out, boundary.id);
        append_flag(out, "fallback", boundary.fallback);
        append_flag(out, "held", boundary.held);
        out.push_back('}');
    }
    out += "],\"error\":";
    append_quoted(out, result.error);
    out += "}\n";
    return out;
}

std::string pending_control_json(PreviewSession* session, std::string_view action,
                                 std::string_view resource, double number, std::string_view text,
                                 std::uint64_t epoch) {
    PendingControlResult result;
    PendingControl control;
    if (session == nullptr) {
        result.error = "no canvas session: call tessera_canvas_apply first";
    } else if (!pending_action_from(action, control.action)) {
        result.error = "unknown resource action `" + std::string(action) +
                       "` (expected fetch, resolve or fail)";
    } else {
        control.resource = std::string(resource);
        control.epoch = epoch;
        control.number = number;
        control.text = std::string(text);
        control.message = std::string(text);
        result = session->control_resource(control);
    }
    return pending_report_json(result,
                               session != nullptr ? session->pending_report() : PendingReport{});
}

} // namespace tessera::compiler::codegen::cli
