// SPDX-License-Identifier: MIT

// Determinism of hook/binding lowering (docs/design/jsx-ir-lowering.md section 7): the component
// data is a pure function of the AST and the analysis, so lowering twice, and at the same time on
// eight threads, gives byte-identical text.

#include "lowering_hooks_test_util.hpp"

#include <array>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace lowering_hooks_test;

constexpr std::string_view kSource =
    "function A({ initial = 0 }: P) {\n"
    "  const [count, setCount] = useState<int>(initial);\n"
    "  const [label, setLabel] = useState(\"idle\");\n"
    "  const r = useRef<View>(null);\n"
    "  const v = useAnimatedValue(1.0);\n"
    "  // Note: bind::on_click\n"
    "  const clicked = (): void => { setCount(1); };\n"
    "  // Note: bind::sync\n"
    "  useEffect(() => { log(initial); log(count); log(v); return () => { stop(); }; }, [initial, "
    "count, v]);\n"
    "  const reset = () => { setCount(0); setLabel(\"idle\"); v.set(0.5, { animate: false }); };\n"
    "  return <View/>;\n"
    "}\n";

std::string describe_source(std::string_view source) {
    const auto fixture = parse(std::string(source));
    const auto result = lower(*fixture);
    if (!result.has_value()) {
        check(false, "the determinism fixture lowers: " + describe_errors(*fixture, result));
        return "<no IR>";
    }
    return describe(*result);
}

void test_two_lowerings_agree() {
    const std::string first = describe_source(kSource);
    const std::string second = describe_source(kSource);
    check(first != "<no IR>", "the determinism fixture lowers");
    check_eq(first, second, "lowering twice gives identical text");
    check(!first.empty() && first.back() == '\n', "the description ends with LF");
    check(first.find('\r') == std::string::npos, "the description has no CR");
    // The sorted-by-id bindings are the part order could most easily leak into.
    check(first.find("binding bind::on_click") < first.find("binding bind::sync") &&
              first.find("binding bind::sync") < first.find("binding bind::sync_cleanup"),
          "bindings are sorted by id in the output");
}

void test_same_fixture_lowers_twice() {
    const auto fixture = parse(std::string(kSource));
    const auto first = lower(*fixture);
    const auto second = lower(*fixture);
    check(first.has_value() && second.has_value(), "the same analysis lowers twice");
    if (first.has_value() && second.has_value()) {
        check_eq(describe(*first), describe(*second),
                 "lowering does not consume or change the analysis");
    }
}

void test_eight_threads_agree() {
    constexpr std::size_t kThreads = 8;
    std::array<std::string, kThreads> texts;
    std::array<std::thread, kThreads> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&texts, i] { texts[i] = describe_source(kSource); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 1; i < kThreads; ++i) {
        check_eq(texts[i], texts[0], "concurrent lowering agrees with thread 0");
    }
}

} // namespace

int main() {
    test_two_lowerings_agree();
    test_same_fixture_lowers_twice();
    test_eight_threads_agree();
    return finish("lowering hooks determinism tests");
}
