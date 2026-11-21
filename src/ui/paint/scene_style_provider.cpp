// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint {

SceneStyleProvider::SceneStyleProvider(const style::CompiledStyleSheet& sheet,
                                       style::SceneStyleTree& tree)
    : sheet_(&sheet),
      tree_(&tree) {}

style::ComputedStyle SceneStyleProvider::compute(const Node& node) {
    return tree_->compute(*sheet_, node);
}

const style::ComputedStyle& SceneStyleProvider::resolve(const Node& node) {
    return tree_->memo_active() ? tree_->resolve(*sheet_, node) : StyleProvider::resolve(node);
}

void SceneStyleProvider::begin_restyle() {
    tree_->begin_memo();
}

void SceneStyleProvider::end_restyle() {
    tree_->end_memo();
}

} // namespace tessera::ui::paint
