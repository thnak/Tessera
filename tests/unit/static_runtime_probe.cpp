// SPDX-License-Identifier: MIT

// The sample framework-linking executable of the release gates (ADR-012, issue #219). Three scripts
// used the deleted `merger_tests` for this: `check_static_runtime.py` (prove a Release artifact
// links the CRT statically), `check_reproducible.py` (two builds produce byte-identical archives of
// the same executable) and `release_rehearsal.py` (the staged package ships a runnable binary).
//
// The probe is deliberately tiny and deterministic: it touches one type from each of the layers a
// shipped app links — the scene graph, the fixed-step runtime and the stylesheet tokenizer — so the
// static-link proof covers real framework code rather than a stub. It prints one line and exits 0.

#include "tessera/runtime/fixed_step.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main() {
    tessera::ui::Node root(1);
    auto child = std::make_unique<tessera::ui::Node>(2);
    child->set_element_kind("Text");
    root.add_child(std::move(child));

    tessera::runtime::FixedStepLoop loop;
    int steps = 0;
    for (int frame = 0; frame < 10; ++frame) {
        steps += static_cast<int>(loop.advance(1.0 / 60.0).steps);
    }

    const std::string css = "a { color: red; }";
    tessera::ui::style::Tokenizer tokenizer(css);
    const std::vector<tessera::ui::style::Token> tokens = tokenizer.tokenize();
    if (root.children().size() != 1 || steps != 10 || tokens.empty()) {
        std::cerr << "static_runtime_probe: unexpected result\n";
        return 1;
    }
    std::cout << "static_runtime_probe: ok (" << tokens.size() << " tokens, " << steps
              << " fixed steps)\n";
    return 0;
}
