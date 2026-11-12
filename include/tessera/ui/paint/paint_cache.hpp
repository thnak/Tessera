// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
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
//   7. the node's own dirty flags are Clean, i.e. nothing marked it since the paint that recorded
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
    std::size_t transforms = 0;
};

// One recorded sink call. Fields are meaningful per kind (see Kind); the unused ones stay default.
struct PaintOp {
    enum class Kind : std::uint8_t {
        FillRect,
        FillRoundedRect,
        FillLinearGradient,
        PushClip,
        PushTransform,
        DrawText,
        DrawVector,
        DrawImage,
    };

    Kind kind = Kind::FillRect;
    rhi::Rect rect;        // FillRect, FillRoundedRect, DrawText, DrawVector, DrawImage
    rhi::Color color;      // FillRect, FillRoundedRect, DrawText, tint of DrawVector/DrawImage
    float radius = 0.0f;   // FillRoundedRect
    RoundedClip clip;      // PushClip
    rhi::Mat3 transform;   // PushTransform
    GradientDraw gradient; // FillLinearGradient (its own rect/clip/stops are authoritative)
    std::string text;      // DrawText text bytes, or the borrowed asset name of DrawVector/Image
    float font_size = 0.0f;
    int font_weight = 400;
    std::optional<float> baseline;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    bool decode_original = false;
};

// The replayed value of one node: everything the node emitted before its children, plus the
// push/pop state that brackets them. Pops are stored as flags rather than ops so the pass can emit
// the prefix, walk the children (which may insert/evict entries), and then emit the pops without
// holding a pointer into the cache.
struct PaintRecord {
    std::vector<PaintOp> ops; // emitted before the children, in exact sink order
    bool pushed_transform = false;
    bool pushed_clip = false;
    RoundedClip clip; // valid when pushed_clip; also feeds the children's clip fingerprint
    PaintCounts counts;
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

    // Context inputs that change emitted values.
    std::uint32_t metrics_source_id = 0;
    std::uint32_t device_scale_bits = 0;
};

// Seed of the deterministic FNV-1a clip-stack fingerprint. A paint starts from this value; every
// pushed clip is folded in with extend_paint_clip_hash().
inline constexpr std::uint64_t kPaintClipHashSeed = 1469598103934665603ull;

[[nodiscard]] std::uint64_t extend_paint_clip_hash(std::uint64_t seed, const RoundedClip& clip);

// Emits recorded ops into `sink` in order. Text/vector/image ops rebuild their draw structs from
// the record's owned strings, so the replay is independent of the scene's lifetime.
void replay_paint_ops(const std::vector<PaintOp>& ops, PaintSink& sink);

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
        explicit Recorder(PaintSink& target);

        void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override;
        void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                               float radius) override;
        void fill_linear_gradient(const GradientDraw& draw) override;
        void push_clip(const RoundedClip& clip) override;
        void pop_clip() override;
        [[nodiscard]] bool supports_transform() const override;
        void push_transform(const rhi::Mat3& transform) override;
        void pop_transform() override;
        void draw_text(const TextDraw& run) override;
        void draw_vector(const VectorDraw& draw) override;
        void draw_image(const ImageDraw& draw) override;

        // Returns the captured record. The recorder must not be reused afterwards.
        [[nodiscard]] PaintRecord finish();

    private:
        void record(PaintOp op);

        PaintSink& target_;
        std::vector<PaintOp> ops_;
        RoundedClip clip_;
        bool pushed_clip_ = false;
        bool pushed_transform_ = false;
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
