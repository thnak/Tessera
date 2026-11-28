// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/io_task_executor.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace tessera::ui::text {

// Rasterizes a set of glyphs on an executor thread so a render-scale step does not stall the UI
// thread (issue #948; the cost is measured in docs/design/text-gpu-raster.md section 2).
//
// Flow, all state changes on the UI thread:
//   1. `start()` hands the glyph keys to the executor and returns at once. Nothing is rasterized
//      inline.
//   2. The worker clones each face (`FontFace::clone_independent`, its own FreeType library, so it
//      never touches the UI thread's rasterizers) and rasterizes the keys into private bitmaps.
//   3. The executor's completion marks the job ready on the UI thread. `ready()` is a flag read.
//   4. `install()` copies the finished bitmaps into an atlas through
//      `GlyphAtlas::insert_prerasterized`, which places them exactly like a cache miss, so the
//      pixels equal what rasterizing on demand would have produced.
//
// No UI-thread wait anywhere: a job that is not ready is simply not installed yet, and the caller
// keeps drawing from the atlas it has. A glyph that was not prewarmed (or did not fit) is
// rasterized on demand as always, so the prewarm is a latency hint, never a correctness input.
//
// `cancel()` and a newer `start()` abandon the job: the worker notices at the next glyph and
// stops, and its result is never installed. The worker owns everything it touches (the job state
// is shared with the closure), so destroying this object while a job runs is safe and nothing is
// detached: the executor owns the thread and joins it.
class GlyphPrewarm {
public:
    GlyphPrewarm();
    ~GlyphPrewarm();

    GlyphPrewarm(const GlyphPrewarm&) = delete;
    GlyphPrewarm& operator=(const GlyphPrewarm&) = delete;

    // Starts rasterizing `keys` (face i of a key is `rasterizers[i]`'s face) with the given
    // hinting and AA mode, abandoning any job in flight. False when the executor refuses the task
    // (at capacity or stopping) or `keys` is empty; no job is pending then.
    bool start(runtime::IoTaskExecutor& executor, std::span<GlyphRasterizer* const> rasterizers,
               std::vector<GlyphAtlasKey> keys, GlyphHinting hinting, GlyphAaMode aa_mode);

    void cancel() noexcept;

    // A job was started and has not been installed or cancelled.
    [[nodiscard]] bool pending() const noexcept;
    // The pending job's worker finished and its completion ran: `install()` will place glyphs.
    [[nodiscard]] bool ready() const noexcept;

    // Places the finished glyphs into `atlas` in key order and ends the job. Returns how many
    // glyphs are cached afterwards. 0 and no effect while the job is not ready.
    std::size_t install(GlyphAtlas& atlas);

private:
    struct Job;
    static void run(Job& job);
    std::shared_ptr<Job> job_;
};

} // namespace tessera::ui::text
