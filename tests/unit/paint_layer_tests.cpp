// SPDX-License-Identifier: MIT
//
// The isolated-layer sink seam (issue #227, docs/design/gpu/offscreen-layers.md section 5.2): the
// default is "no layers" so every existing sink keeps painting groups flat; a wrapper that cannot
// replay a layer per damage region says so; a recorder captures a layer as a push op plus a flag (a
// per-node record) or as push and pop ops (the subtree draw list), and a replay reproduces it.

#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using namespace tessera::ui::paint;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_layer: " << message << "\n";
    }
    return condition;
}

// A sink with no layer support: the defaults, and nothing else, so a call that reached it would be
// visible as a log entry only if the base class forwarded it.
class FlatSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        log.push_back("fill");
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        log.push_back("fill");
    }
    std::vector<std::string> log;
};

class LayerSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        log.push_back("fill");
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        log.push_back("fill");
    }
    [[nodiscard]] bool supports_layers() const override {
        return true;
    }
    void push_layer(const LayerDraw& layer) override {
        log.push_back("push");
        last = layer;
    }
    void pop_layer() override {
        log.push_back("pop");
    }
    std::vector<std::string> log;
    LayerDraw last;
};

const LayerDraw kLayer{{10.0f, 20.0f, 30.0f, 40.0f}, 0.5f};
const Rect kBox{0.0f, 0.0f, 4.0f, 4.0f};
const Color kColor{1.0f, 0.0f, 0.0f, 1.0f};

bool same(const LayerDraw& a, const LayerDraw& b) {
    return a.box.x == b.box.x && a.box.y == b.box.y && a.box.width == b.box.width &&
           a.box.height == b.box.height && a.opacity == b.opacity;
}

bool test_default_sink_has_no_layers() {
    FlatSink sink;
    sink.push_layer(kLayer);
    sink.pop_layer();
    return expect(!sink.supports_layers(), "the default sink does not support layers") &&
           expect(sink.log.empty(), "the default hooks are no-ops");
}

bool test_clipped_sink_reports_no_layers() {
    LayerSink target;
    const ClippedPaintSink clipped(target, std::vector<Scissor>{});
    return expect(!clipped.supports_layers(),
                  "a damage-replay wrapper reports no layers even over a layer-capable target");
}

bool test_recorder_forwards_support_and_calls() {
    LayerSink target;
    PaintCache::Recorder recorder(target);
    recorder.push_layer(kLayer);
    recorder.fill_rect(kBox, kColor);
    recorder.pop_layer();
    return expect(recorder.supports_layers(), "the recorder forwards the target's support") &&
           expect((target.log == std::vector<std::string>{"push", "fill", "pop"}),
                  "the recorder forwards push, draw and pop in order") &&
           expect(same(target.last, kLayer), "the layer arrives unchanged");
}

bool test_per_node_record_stores_push_and_flag() {
    LayerSink target;
    PaintCache::Recorder recorder(target); // per-node: pops are flags
    recorder.push_layer(kLayer);
    recorder.fill_rect(kBox, kColor);
    recorder.pop_layer();
    const PaintRecord record = recorder.finish();
    if (!expect(record.pushed_layer, "the record carries the pushed-layer flag") ||
        !expect(record.ops.size() == 2 && record.ops[0].kind == PaintOp::Kind::PushLayer &&
                    record.ops[1].kind == PaintOp::Kind::FillRect,
                "a per-node record holds the push op and no pop op")) {
        return false;
    }
    LayerSink replayed;
    replay_paint_ops(record.ops, record.gradients, replayed);
    return expect((replayed.log == std::vector<std::string>{"push", "fill"}),
                  "a replay pushes the layer; the pass closes it after the children") &&
           expect(same(replayed.last, kLayer), "the replayed layer is the recorded one");
}

bool test_subtree_record_stores_explicit_pop() {
    LayerSink target;
    PaintCache::Recorder recorder(target, true); // subtree draw list: pops are ops
    recorder.push_layer(kLayer);
    recorder.fill_rect(kBox, kColor);
    recorder.pop_layer();
    const PaintRecord record = recorder.finish();
    if (!expect(record.ops.size() == 3 && record.ops[2].kind == PaintOp::Kind::PopLayer,
                "the subtree record holds an explicit pop")) {
        return false;
    }
    LayerSink replayed;
    replay_paint_ops(record.ops, record.gradients, replayed);
    return expect((replayed.log == std::vector<std::string>{"push", "fill", "pop"}),
                  "a subtree replay is balanced");
}

bool test_record_without_layer_is_unchanged() {
    FlatSink target;
    PaintCache::Recorder recorder(target);
    recorder.fill_rect(kBox, kColor);
    const PaintRecord record = recorder.finish();
    return expect(!record.pushed_layer && record.ops.size() == 1,
                  "a node without a layer records exactly what it did before");
}

} // namespace

int main() {
    const bool ok =
        test_default_sink_has_no_layers() && test_clipped_sink_reports_no_layers() &&
        test_recorder_forwards_support_and_calls() && test_per_node_record_stores_push_and_flag() &&
        test_subtree_record_stores_explicit_pop() && test_record_without_layer_is_unchanged();
    if (!ok) {
        return 1;
    }
    std::cout << "paint_layer: ok\n";
    return 0;
}
