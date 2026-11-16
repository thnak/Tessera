// SPDX-License-Identifier: MIT

#pragma once

// Shared fixtures and helpers for the dynamic-child preview tests (issue #389). Split out of
// `scene_dynamic_tests.cpp` so that file stays inside the source-size budget, following the
// `scene_canvas_test_support.hpp` pattern: the sources are data, and one test file asserts on them.
//
// Every source here is written in the dialect the real front end accepts, which is not the same as
// the shape the issue text uses: `useState<int>(0)` for a scalar, `useState<string[]>([...])` for a
// list, no `import` line, and a ternary (not two `&&` children) when a case needs both branches of
// one conditional to exist as complementary siblings.

#include "scene_prepare.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scene_dynamic_test {

namespace cli = tessera::compiler::codegen::cli;
namespace automation = tessera::ui::automation;

inline constexpr std::string_view kFile = "ui/Dynamic.tsx";

inline cli::ScenePreviewOptions options_for(std::string_view file) {
    cli::ScenePreviewOptions options;
    options.file = file;
    options.salt = "preview";
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    return options;
}

// A ternary: lowering emits two sibling nodes with complementary conditions, so both branches exist
// in the IR and exactly one is painted. `count` starts at 0, so the false branch is the shown one.
inline constexpr std::string_view kConditional = R"(export function Toggle(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Count: {count}</Text>
      {count > 0 ? <Text>Positive</Text> : <Text>Empty</Text>}
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";

// The `map` shape: one `Text` per item of a `list<string>` cell, keyed by the loop index so the
// item key is numeric and both the preview and generated code publish it.
inline constexpr std::string_view kRepeated = R"(export function Rows(): Node {
  const [rows, setRows] = useState<string[]>(["alpha", "beta", "gamma"]);
  return (
    <View>
      {rows.map((row, i) => <Text key={i}>{row}</Text>)}
    </View>
  );
}
)";

// The empty list: the template contributes no item, but the container and the sibling survive.
inline constexpr std::string_view kEmptyList = R"(export function Empty(): Node {
  const [rows, setRows] = useState<string[]>([]);
  return (
    <View>
      {rows.map((row, i) => <Text key={i}>{row}</Text>)}
      <Text>After</Text>
    </View>
  );
}
)";

// A repeated child followed by a sibling: the difference between "the list contributed nothing" and
// "the walk stopped at the list".
inline constexpr std::string_view kListThenSibling = R"(export function One(): Node {
  const [rows, setRows] = useState<string[]>(["only"]);
  return (
    <View>
      {rows.map((row, i) => <Text key={i}>{row}</Text>)}
      <Text>After</Text>
    </View>
  );
}
)";

// A hidden branch whose subtree the preview cannot build if it were shown (a nested list template,
// which the preview defers). The shown tree must still build.
inline constexpr std::string_view kHiddenBranch = R"(export function Hidden(): Node {
  const [open, setOpen] = useState<boolean>(false);
  const [rows, setRows] = useState<string[]>(["a"]);
  return (
    <View>
      <Text>Base</Text>
      {open && <View>{rows.map((row, i) => <View key={i}>{rows.map((cell, j) => <Text key={j}>{cell}</Text>)}</View>)}</View>}
    </View>
  );
}
)";

// The same source with the condition inverted, so the deferred branch *is* the shown one. Without
// this complement the hidden-branch case would pass for a preview that never builds any branch at
// all.
inline constexpr std::string_view kShownBranch = R"(export function Shown(): Node {
  const [open, setOpen] = useState<boolean>(true);
  const [rows, setRows] = useState<string[]>(["a"]);
  return (
    <View>
      <Text>Base</Text>
      {open && <View>{rows.map((row, i) => <View key={i}>{rows.map((cell, j) => <Text key={j}>{cell}</Text>)}</View>)}</View>}
    </View>
  );
}
)";

// Every non-empty text payload in document order: what a page would paint.
inline std::vector<std::string> texts(const automation::UiSnapshot& snapshot) {
    std::vector<std::string> out;
    for (const automation::UiSnapshotNode& node : snapshot.nodes) {
        if (!node.text.empty()) {
            out.push_back(node.text);
        }
    }
    return out;
}

inline bool has_text(const std::vector<std::string>& haystack, std::string_view needle) {
    for (const std::string& text : haystack) {
        if (text == needle) {
            return true;
        }
    }
    return false;
}

inline std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += " | ";
        }
        out += item;
    }
    return out;
}

// A snapshot of a prepared scene, or an empty one with `error` set, so every case reports a failed
// prepare the same way.
inline automation::UiSnapshot snapshot_of(const cli::PreparedScene& scene, std::string& error) {
    error = scene.error;
    if (!scene.error.empty() || scene.root == nullptr) {
        return {};
    }
    auto snapshot = automation::snapshot_scene(*scene.root);
    if (!snapshot) {
        error = "the snapshot walk refused the tree (code " +
                std::to_string(static_cast<int>(snapshot.error().code)) + ")";
        return {};
    }
    return *snapshot;
}

} // namespace scene_dynamic_test
