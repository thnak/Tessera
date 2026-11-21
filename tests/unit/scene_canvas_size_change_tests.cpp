// SPDX-License-Identifier: MIT
//
// `onSizeChange` in the Studio canvas session (ADR-041 item 3, issue #694; the contract is
// docs/specs/size-change-events-v1.md). The preview is an IR evaluator, so it cannot run the
// developer's C++ thunk: it runs the same IR body the generated base would, with a
// `SizeChangeEvent` record bound to the handler's parameter. This test proves the callback fires on
// the first layout and after a viewport change, that the payload carries the real box, and that a
// callback which changes state re-evaluates the tree in the same call.

#include "scene_canvas_test_support.hpp"

#include <string>
#include <string_view>

namespace {

using canvas_test::expect;
using canvas_test::source_for;

// A panel that records its own width in a state cell, so the assertion is on the state the callback
// wrote rather than on a log. `wide` flips at 400 px, which is the same shape the native
// `codegen_size_change_runtime` test drives.
constexpr std::string_view kSizeWatcher = R"(export function SizeWatcher(): Node {
  const [wide, setWide] = useState<bool>(false);
  const [seen, setSeen] = useState<int>(0);
  return (
    <View className="panel" onSizeChange={(e) => { setSeen(seen + 1); setWide(e.width >= 400); }}>
      <Text>{wide ? "wide" : "narrow"}</Text>
    </View>
  );
}
)";

bool state_bool(const tessera::compiler::codegen::cli::CanvasSceneSession& session,
                std::string_view name) {
    for (const tessera::ui::StateCell& cell : session.state()) {
        if (cell.name != name) {
            continue;
        }
        if (const bool* value = std::get_if<bool>(&cell.value)) {
            return *value;
        }
    }
    return false;
}

void first_layout_delivers() {
    tessera::compiler::codegen::cli::CanvasSceneSession session;
    const std::string error = session.start(source_for(kSizeWatcher, "ui/SizeWatcher.tsx"));
    expect(error.empty(), "the source evaluates: " + error);
    // The callback ran on the first layout, so the state cell it wrote is no longer its initial
    // value. A 320 px viewport is below the 400 px condition.
    expect(canvas_test::counter_value(session, "state::seen") == 1,
           "onSizeChange fired once on the first layout");
    expect(!state_bool(session, "state::wide"), "320 px is below the 400 px condition");
}

void viewport_change_delivers_again() {
    tessera::compiler::codegen::cli::CanvasSceneSession session;
    const std::string error = session.start(source_for(kSizeWatcher, "ui/SizeWatcher.tsx"));
    expect(error.empty(), "the source evaluates: " + error);
    const int before = canvas_test::counter_value(session, "state::seen");
    // A viewport change re-lays the running tree out and runs the settle loop in the same call, so
    // the callback sees the new width before `set_viewport` returns.
    expect(session.set_viewport(500.0f, 240.0f, 1.0f), "the viewport change lays out");
    expect(canvas_test::counter_value(session, "state::seen") > before,
           "onSizeChange fired again after the resize");
    expect(state_bool(session, "state::wide"), "500 px crosses the 400 px condition");
}

void same_size_delivers_nothing() {
    tessera::compiler::codegen::cli::CanvasSceneSession session;
    const std::string error = session.start(source_for(kSizeWatcher, "ui/SizeWatcher.tsx"));
    expect(error.empty(), "the source evaluates: " + error);
    const int before = canvas_test::counter_value(session, "state::seen");
    // The same viewport is not a layout input change, so no pass runs and nothing is delivered.
    expect(!session.set_viewport(320.0f, 240.0f, 1.0f), "an unchanged viewport is no pass");
    expect(canvas_test::counter_value(session, "state::seen") == before,
           "an unchanged size delivers nothing");
}

void settle_counters_are_reported() {
    tessera::compiler::codegen::cli::CanvasSceneSession session;
    const std::string error = session.start(source_for(kSizeWatcher, "ui/SizeWatcher.tsx"));
    expect(error.empty(), "the source evaluates: " + error);
    // The session exposes the same counters `AppHost` does, so a page can report what the settle
    // loop did instead of guessing.
    expect(session.settle_totals().deliveries >= 1, "the settle loop counted a delivery");
}

} // namespace

int main() {
    first_layout_delivers();
    viewport_change_delivers_again();
    same_size_delivers_nothing();
    settle_counters_are_reported();
    return canvas_test::failures() != 0 ? 1 : 0;
}
