// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace tessera::ui::paint {

// Caller-owned per-node paint cache (the "per-node paint caches" half of the dirty-rendering item).
// It owns the exact sequence of PaintSink calls one node emits for itself, the node's own clip and
// transform push/pop, and the PaintStats deltas that emission produced.
//
// Granularity is PER NODE: a record covers the node's own draws only, never its children's. The
// paint pass still walks every child; each child hits or records its own entry. A record therefore
// never has to be merged with descendant state, and a dirty descendant cannot leak a stale parent
// record into the frame. Coordinates are stored verbatim: the key pins the sink-space mapping
// (painted matrix, painting frame, resolved box, inherited clip stack), so a moved, transformed or
// re-clipped node MISSES and re-records instead of being replayed at adjusted coordinates. That is
// the deliberate trade - the acceptance is byte-identical sink output, not a hit rate.
//
// An entry is replayed only when every one of these matches:
//   1. the node id (stable scene identity),
//   2. the paint-relevant node state the pass reads without resolving style (bounds, transform,
//      scroll offset, scrollable axes, scrollport insets),
//   3. the sink-space mapping (painted world matrix, painted frame, resolved box, whether the sink
//      supports transforms),
//   4. the active clip stack entering the node (a deterministic FNV-1a fingerprint of every pushed
//      clip's rect and radii plus the depth),
//   5. the context (text metrics source id, device scale),
//   6. the exact PaintData payload, compared by value rather than by address,
//   7. the effective opacity inherited from the ancestors, because a record's colours already have
//      the product folded in (an ancestor fade must re-record the whole subtree, see
//      docs/design/paint-pass.md section 14.1),
//   8. the node's own dirty flags are Clean, i.e. nothing marked it since the paint that recorded
//      it. The dirty flags are the style-generation signal: a style, cascade or theme change marks
//      the node and invalidates its record without the cache having to resolve the style first.
//      A caller that swaps a style generation without marking affected nodes must call
//      invalidate().
//
// Deliberate bypasses (counted as stats.bypasses; never a silent stale replay):
//   - PaintContext::view != nullptr: a PaintView can override colors, opacity and transforms per
//     frame in a way the pass cannot fingerprint without applying it, and text metrics can depend
//     on that override, so a paint with a view installed takes the uncached path end to end.
//   - The sink's supports_transform() is part of the key, so switching sinks misses.
//   - Box shadows need no bypass in this codebase: the blur layer count/radii derive from the
//     computed style only (no sink device scale or backing size), which the dirty flag covers.
//   - Non-finite geometry can never replay: IEEE comparison makes the key unequal, so it misses.
//
// The cache is bounded (entries and bytes), uses deterministic LRU eviction, and is not a global
// singleton: one instance belongs to one scene tree and is passed through PaintContext. Call
// invalidate() when that tree is rebuilt or the stylesheet/theme generation changes.
struct PaintCacheConfig {
    // Both budgets are enforced; an entry that does not fit even in an empty cache is not stored.
    // Entries are evicted least-recently-used first, in the same order on every run. Byte
    // accounting (paint_cache.cpp): sizeof(Entry) + ops.capacity() * sizeof(PaintOp) + the copied
    // PaintData string capacity + every op's copied text/asset capacity + the LRU link overhead.
    std::size_t max_entries = 256;
    std::size_t max_bytes = 2 * 1024 * 1024;
};

struct PaintCacheStats {
    std::uint64_t hits = 0;           // lookups that replayed a record
    std::uint64_t misses = 0;         // no valid record (or the node was marked dirty)
    std::uint64_t evictions = 0;      // entries dropped by the entry/byte budget
    std::uint64_t invalidations = 0;  // invalidate() calls (style/tree generation changes)
    std::uint64_t replayed_draws = 0; // sink calls replayed from records
    std::uint64_t recorded_draws = 0; // sink calls captured while recording
    std::uint64_t bypasses = 0;       // nodes painted with the cache bypassed (PaintView set)
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

// Draw counters of one record: the PaintStats fields the node's own emission increments. They are
// replayed so a fully cached paint reports the same draw composition as a cold paint (only
// PaintStats::nodes drops, because no style was resolved).
struct PaintCounts {
    std::size_t backgrounds = 0;
    std::size_t borders = 0;
    std::size_t clips = 0;
    std::size_t texts = 0;
    std::size_t vectors = 0;
    std::size_t images = 0;
    std::size_t gradients = 0;
    std::size_t shadows = 0;
    std::size_t text_shadows = 0;
    std::size_t transforms = 0;
    std::size_t backdrops = 0;
};

// The payload of one recorded sink call, one alternative per kind (issue #731).
//
// Why a variant and not the flat struct this used to be: the payloads are mutually exclusive by
// kind, so a flat struct paid for all of them at once. `GradientDraw` alone is 704 bytes and the
// radial/conic forms 712 each, which made every op 2376 bytes -- a `FillRect` carried 33 useful
// bytes in 2376. The paint cache charges `ops.capacity() * sizeof(PaintOp)` against a 2 MiB budget
// (paint_cache.cpp `entry_bytes`), so the flat shape held only 104 entries of the configured 256.
//
// The three gradient payloads stay OUT of the variant, referenced by index into
// `PaintRecord::gradients`. A variant holds its largest alternative inline, so putting them in
// would only shrink the op to 864 bytes; the side table is what makes the common op small. A
// gradient is also the rarest kind, so the indirection costs nothing on the hot path.
struct FillRectOp {
    rhi::Rect rect;
    rhi::Color color;
};

struct FillRoundedRectOp {
    rhi::Rect rect;
    rhi::Color color;
    float radius = 0.0f;
};

struct FillRoundedRingOp {
    rhi::Rect rect;
    rhi::Color color;
    float radius = 0.0f;
    float width = 0.0f;
};

struct FillBoxShadowOp {
    rhi::Rect box;
    RoundedClip carve; // the node's border box shaped by its border-radius
    rhi::Color color;
    float radius = 0.0f;
    float sigma = 0.0f;
    bool inset = false; // an inset shadow (issue #612)
};

// A gradient payload, held in `PaintRecord::gradients` rather than inline. The draw's own rect and
// clip are authoritative, exactly as the sink's `GradientDraw` documents.
struct GradientRefOp {
    std::uint32_t index = 0;
};

// The three gradient shapes, one alternative each. They are 704-712 bytes apiece, which is why they
// live in a side table instead of inside `PaintOp`.
using GradientPayload = std::variant<GradientDraw, RadialGradientDraw, ConicGradientDraw>;

struct PushClipOp {
    RoundedClip clip;
};

struct PushTransformOp {
    rhi::Mat3 transform;
};

struct DrawTextOp {
    rhi::Rect rect;
    rhi::Color color;
    float font_size = 0.0f;
    int font_weight = 400;
    std::optional<float> baseline;
    bool lcd = false;
    std::string text; // the run's UTF-8 bytes
};

// DrawVector and DrawImage share one shape: both carry a borrowed asset name, an optional raster
// size, a scale hint and a tint. `decode_original` is meaningful for an image only.
struct DrawAssetOp {
    rhi::Rect rect;
    rhi::Color tint;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    bool decode_original = false;
    std::string asset;
};

struct DrawBackdropOp {
    BackdropDraw draw;
};

struct PushLayerOp {
    LayerDraw layer;
};

struct RetainLayerOp {
    LayerRetention retention;
};

// One recorded sink call: the kind tag plus the payload that kind owns. `kind` is kept beside the
// variant rather than derived from the active alternative because the three gradient kinds share
// one alternative and the two asset kinds share another.
struct PaintOp {
    enum class Kind : std::uint8_t {
        FillRect,
        FillRoundedRect,
        FillRoundedRing,
        FillBoxShadow,
        FillLinearGradient,
        FillRadialGradient,
        FillConicGradient,
        PushClip,
        PushTransform,
        PushLayer,
        // Recorded only with `record_pops`, right before the layer's PushLayer: a replay asks the
        // sink whether it holds the layer and, if so, skips everything up to the matching PopLayer.
        RetainLayer,
        DrawText,
        DrawVector,
        DrawImage,
        DrawBackdrop,
        // Recorded only by a Recorder built with `record_pops` (the subtree draw list, which must
        // carry its own pops); a per-node record stores pops as flags and never holds these.
        PopClip,
        PopTransform,
        PopLayer,
        // Recorded only with `record_pops`: a masked node paints its mask after its children, which
        // a per-node record cannot hold, so such a node is never per-node recorded and its mask
        // lives in the subtree draw list as Begin, the mask's gradient fills, End (issue #617).
        BeginLayerMask,
        EndLayerMask,
        // A non-add mask layer inside a recorded mask (the operator is the kind, so no payload).
        BeginMaskSubtract,
        BeginMaskIntersect,
        BeginMaskExclude,
        EndMaskLayer,
    };

    using Payload = std::variant<FillRectOp, FillRoundedRectOp, FillRoundedRingOp, FillBoxShadowOp,
                                 GradientRefOp, PushClipOp, PushTransformOp, DrawTextOp,
                                 DrawAssetOp, DrawBackdropOp, PushLayerOp, RetainLayerOp>;

    Kind kind = Kind::FillRect;
    Payload payload = FillRectOp{};
};

// The replayed value of one node: everything the node emitted before its children, plus the
// push/pop state that brackets them. Pops are stored as flags rather than ops so the pass can emit
// the prefix, walk the children (which may insert/evict entries), and then emit the pops without
// holding a pointer into the cache.
struct PaintRecord {
    std::vector<PaintOp> ops; // emitted before the children, in exact sink order
    // The gradient payloads the ops reference by index (issue #731). Kept out of `PaintOp` so a
    // `FillRect` does not pay for a 704-byte gradient it never carries; a record with no gradient
    // op leaves this empty and allocates nothing.
    std::vector<GradientPayload> gradients;
    bool pushed_transform = false;
    bool pushed_clip = false;
    // The node opened an isolated layer (issue #227): the pass closes it after the child walk,
    // after the clip pop and before the transform pop, like the other pops.
    bool pushed_layer = false;
    RoundedClip clip; // valid when pushed_clip; also feeds the children's clip fingerprint
    PaintCounts counts;
    // Effective opacity this node composited its own draws with and passes to its descendants
    // (inherited product x the node's own `opacity`). A replay walks the children without
    // resolving this node's style, so the value is stored instead of recomputed. The signature's
    // `opacity_bits` pins the inherited half, and the node's own style change invalidates the
    // record through its dirty flags.
    float effective_opacity = 1.0f;
};

// Everything that can change what one node emits, excluding the computed style (represented by the
// node's dirty flags) and the PaintData payload (compared by value).
struct PaintCacheSignature {
    NodeId node = 0;

    // Paint-relevant node state read without resolving style.
    rhi::Rect bounds;
    rhi::Mat3 transform;
    bool scrollable_x = false;
    bool scrollable_y = false;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    float scrollport_left = 0.0f;
    float scrollport_top = 0.0f;
    float scrollport_right = 0.0f;
    float scrollport_bottom = 0.0f;

    // Sink-space mapping the record's coordinates were produced in.
    bool interpolated = false;
    rhi::Mat3 painted_world;
    bool frame_active = false;
    rhi::Mat3 frame_inverse;
    bool sink_supports_transform = false;
    rhi::Rect paint_rect;

    // Active clip stack entering the node.
    std::uint64_t clip_hash = 0;
    std::uint32_t clip_depth = 0;

    // Effective opacity inherited from the ancestors (the product of their `opacity` values,
    // excluding this node's own). Floats are compared by IEEE bit pattern: a record's colours are
    // replayed verbatim, so an ancestor fade must never hit a record folded with another product.
    std::uint32_t opacity_bits = 0;

    // Context inputs that change emitted values.
    std::uint32_t metrics_source_id = 0;
    std::uint32_t device_scale_bits = 0;
    bool reduce_effects = false;
    // Glass (issue #244): high-contrast mode suppresses `backdrop-filter` entirely, and the two
    // limits change the emitted sigma / half-resolution decision, so all three are part of the key
    // - exactly like reduce_effects, because they change the commands a record holds.
    bool high_contrast = false;
    // PaintContext::lcd_text: a recorded text op carries `TextDraw::lcd`, which that permission
    // decided, so a record made under the other value must not replay.
    bool lcd_text = false;
    // PaintContext::isolate_group_opacity on a sink that supports layers: a record made with a
    // layer op (and a different opacity fold) must not replay when the other holds.
    bool isolate_groups = false;
    // The node paints inside an isolated layer: its text stays grayscale, so a record made with LCD
    // eligibility must not replay there (an ancestor's own opacity does not change this node's
    // inherited product once it is isolated, so the opacity bits alone cannot tell).
    bool in_layer = false;
    float glass_max_blur_radius = 0.0f;
    std::uint64_t glass_max_backdrop_area = 0;
    // The counter of a list item's ordered marker (its position among the parent's list items,
    // moved by the list's `start`/`reversed` and the item's `value`); `list_item` is false for any
    // other node. It depends on siblings and the parent, not on the node, so it is part of the key.
    std::int32_t list_ordinal = 0;
    bool list_item = false;
};

// Seed of the deterministic FNV-1a clip-stack fingerprint. A paint starts from this value; every
// pushed clip is folded in with extend_paint_clip_hash().
inline constexpr std::uint64_t kPaintClipHashSeed = 1469598103934665603ull;

// Exact signature equality (floats by value, so NaN never matches): shared with the subtree draw
// list, whose root key is a PaintCacheSignature.
[[nodiscard]] bool same_paint_signature(const PaintCacheSignature& lhs,
                                        const PaintCacheSignature& rhs);

[[nodiscard]] std::uint64_t extend_paint_clip_hash(std::uint64_t seed, const RoundedClip& clip);

// Emits recorded ops into `sink` in order. Text/vector/image ops rebuild their draw structs from
// the record's owned strings, so the replay is independent of the scene's lifetime. `gradients` is
// the record's side table, which the gradient ops index into.
void replay_paint_ops(const std::vector<PaintOp>& ops,
                      const std::vector<GradientPayload>& gradients, PaintSink& sink);

class PaintCache {
public:
    explicit PaintCache(PaintCacheConfig config = {});

    PaintCache(const PaintCache&) = delete;
    PaintCache& operator=(const PaintCache&) = delete;

    // Returns the record for `signature` when it is still valid, else nullptr (counts a hit or a
    // miss and refreshes the LRU position). The pointer is valid until the next insert()/erase()/
    // eviction; the pass replays it before walking children, and takes the pop flags by value.
    [[nodiscard]] const PaintRecord* find(const PaintCacheSignature& signature,
                                          const std::optional<PaintData>& payload);

    // Inserts or replaces the node's entry and refreshes its LRU position.
    void insert(PaintCacheSignature signature, const std::optional<PaintData>& payload,
                PaintRecord record);

    // Drops one node's entry without a lookup (a caller that knows the style changed).
    void erase(NodeId node);

    // Whether the cache holds an entry for `node`, and whether it holds none at all. The paint
    // pass uses them to tell a dirty node whose record could go stale (it has one) from a dirty
    // node that can be culled freely (it has none, which is every node of a cold first frame).
    [[nodiscard]] bool contains(NodeId node) const {
        return entries_.find(node) != entries_.end();
    }
    // Owners that may hold a retained layer in the sink (#617): registered when a clean layer asks,
    // forgotten when a dirty one reports. Only these (and their ancestors) need the cull exemption.
    void note_retained_owner(NodeId node, bool holds) {
        if (holds) {
            retained_owners_.insert(node);
        } else if (!retained_owners_.empty()) {
            retained_owners_.erase(node);
        }
    }
    [[nodiscard]] bool is_retained_owner(NodeId node) const {
        return !retained_owners_.empty() && retained_owners_.count(node) != 0;
    }
    [[nodiscard]] bool has_retained_owners() const {
        return !retained_owners_.empty();
    }
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }

    // Drops every entry and counts one invalidation (style/tree generation change).
    void invalidate();
    // Drops every entry without counting an invalidation.
    void clear();

    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;

    // One node painted with the cache bypassed (a PaintView was installed).
    void note_bypass();

    // A lookup that could not even be attempted because the node was marked dirty since the paint
    // that recorded it (the style-generation signal). Counted as a miss.
    void note_miss();

    [[nodiscard]] PaintCacheStats stats() const;

    // Forwards every call to `target` and records it. Pops are forwarded but not recorded: the
    // record stores pushed_transform/pushed_clip instead, so a replay can put the node's children
    // between the prefix and the pops.
    class Recorder final : public PaintSink {
    public:
        explicit Recorder(PaintSink& target, bool record_pops = false);

        void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override;
        void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                               float radius) override;
        void fill_rounded_ring(const rhi::Rect& rect, const rhi::Color& color, float radius,
                               float width) override;
        [[nodiscard]] bool supports_box_shadow() const override;
        void fill_box_shadow(const BoxShadowDraw& draw) override;
        void fill_linear_gradient(const GradientDraw& draw) override;
        void fill_radial_gradient(const RadialGradientDraw& draw) override;
        void fill_conic_gradient(const ConicGradientDraw& draw) override;
        void push_clip(const RoundedClip& clip) override;
        void pop_clip() override;
        [[nodiscard]] bool supports_transform() const override;
        [[nodiscard]] bool supports_backdrop() const override;
        void push_transform(const rhi::Mat3& transform) override;
        void pop_transform() override;
        [[nodiscard]] bool supports_layers() const override;
        void push_layer(const LayerDraw& layer) override;
        void pop_layer() override;
        [[nodiscard]] bool retain_layer(const LayerRetention& retention) override;
        [[nodiscard]] bool begin_layer_mask() override;
        void end_layer_mask() override;
        [[nodiscard]] bool begin_mask_layer(rendering::gpu::MaskCompositeOp op) override;
        void end_mask_layer() override;
        void draw_text(const TextDraw& run) override;
        void draw_vector(const VectorDraw& draw) override;
        void draw_image(const ImageDraw& draw) override;
        void draw_backdrop(const BackdropDraw& draw) override;

        // Returns the captured record. The recorder must not be reused afterwards.
        [[nodiscard]] PaintRecord finish();

    private:
        void record(PaintOp op);

        PaintSink& target_;
        bool record_pops_ = false;
        std::vector<PaintOp> ops_;
        // The gradient payloads the recorded ops reference by index (issue #731). Empty for a node
        // that paints no gradient, so the common record allocates nothing for it.
        std::vector<GradientPayload> gradients_;
        RoundedClip clip_;
        bool pushed_clip_ = false;
        bool pushed_transform_ = false;
        bool pushed_layer_ = false;
        // Between begin_layer_mask() and end_layer_mask() of a subtree recording: the mask's
        // gradient fills are always recorded, and forwarded only while the target agreed to mask.
        bool in_mask_ = false;
        bool mask_live_ = false;
        // False while a non-add mask layer the target declined is open: its draws are not
        // forwarded.
        bool part_live_ = true;
        // A retained layer (issue #617): the target holds the layer's pixels, so what the pass
        // paints inside it is recorded (a later replay may need it) but not forwarded.
        // `retain_pending_` is set between the target's "yes" and the layer's push_layer();
        // `retained_depth_` counts open layers from that push to its matching pop (0 = forwarding).
        bool retain_pending_ = false;
        std::uint32_t retained_depth_ = 0;
    };

private:
    struct Entry {
        PaintCacheSignature signature;
        std::optional<PaintData> payload;
        PaintRecord record;
        std::size_t bytes = 0;
        std::list<NodeId>::iterator lru;
    };

    using Lru = std::list<NodeId>;
    using Map = std::map<NodeId, Entry>;
    std::set<NodeId> retained_owners_;

    static std::size_t entry_bytes(const PaintCacheSignature& signature,
                                   const std::optional<PaintData>& payload,
                                   const PaintRecord& record);
    void evict_one();

    PaintCacheConfig config_;
    bool enabled_ = true;
    Map entries_;
    Lru lru_;
    std::size_t bytes_ = 0;
    PaintCacheStats counters_;
};

} // namespace tessera::ui::paint
