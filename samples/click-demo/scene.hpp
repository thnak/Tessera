// SPDX-License-Identifier: MIT
//
// The demo scene shared by the click-demo capture and window modes: the generated Demo component
// (JSX -> IR -> C++ from samples/compiler-demo/Demo.tsx), a real compiled stylesheet that targets
// the emitted element kinds, and the host's update scheduler.

#pragma once

#include "Demo.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

namespace click_demo {

// The scene root is the window's content node; the generated component hangs below it. The id is
// outside the generated id range (the component owns small ids) so layout/damage never collide.
inline constexpr tessera::ui::NodeId kRootId = 900;

struct DemoScene {
    DemoScene() {
        demo.build(root);
        demo.mount();
        demo.attach_scheduler(scheduler);
    }

    tessera::ui::Node root{kRootId};
    demo::Demo demo;
    tessera::ui::UpdateScheduler scheduler;
};

// A small app stylesheet. Every selector is a type selector on the element kinds the compiler emits
// (`View`, `Text`, `Button`, `TextInput`), which is what a real app writes today; class-name
// selectors would need generated `className` metadata.
inline tessera::ui::style::CompiledStyleSheet demo_stylesheet() {
    return tessera::ui::style::compile_stylesheet(R"(
View {
  width: 300px;
  display: flex;
  flex-direction: column;
  padding: 20px;
  border-radius: 12px;
  background-color: #1e293b;
}
Text {
  font-size: 16px;
  color: #e2e8f0;
  margin-bottom: 8px;
}
Button {
  width: 120px;
  height: 36px;
  margin-bottom: 10px;
  border-radius: 8px;
  background-color: #2563eb;
}
Button:hover {
  background-color: #1d4ed8;
}
Button:active {
  background-color: #1e40af;
}
TextInput {
  width: 220px;
  height: 32px;
  padding: 6px;
  border-width: 1px;
  border-color: #334155;
  border-radius: 6px;
  background-color: #0f172a;
  font-size: 14px;
  color: #e2e8f0;
}
)");
}

} // namespace click_demo
