// SPDX-License-Identifier: MIT

#include "tessera/ui/text/glyph_prewarm.hpp"

#include <atomic>
#include <optional>
#include <utility>

namespace tessera::ui::text {

struct GlyphPrewarm::Job {
    std::vector<FontFace> sources;
    std::vector<GlyphAtlasKey> keys;
    std::vector<GlyphBitmap> bitmaps;
    // One byte per key: 1 once its bitmap exists. A face that cannot be cloned leaves its keys 0.
    std::vector<std::uint8_t> rasterized;
    GlyphHinting hinting = GlyphHinting::Font;
    GlyphAaMode aa_mode = GlyphAaMode::Grayscale;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> completed{false};
};

GlyphPrewarm::GlyphPrewarm() = default;

GlyphPrewarm::~GlyphPrewarm() {
    cancel();
}

bool GlyphPrewarm::start(runtime::IoTaskExecutor& executor,
                         std::span<GlyphRasterizer* const> rasterizers,
                         std::vector<GlyphAtlasKey> keys, GlyphHinting hinting,
                         GlyphAaMode aa_mode) {
    cancel();
    if (keys.empty() || rasterizers.empty()) {
        return false;
    }
    auto job = std::make_shared<Job>();
    job->sources.reserve(rasterizers.size());
    for (const GlyphRasterizer* rasterizer : rasterizers) {
        // A copy shares the immutable state; the worker only reads its bytes.
        job->sources.push_back(rasterizer->face());
    }
    job->keys = std::move(keys);
    job->bitmaps.resize(job->keys.size());
    job->rasterized.assign(job->keys.size(), 0);
    job->hinting = hinting;
    job->aa_mode = aa_mode;

    runtime::IoTask task;
    task.work = [job]() { run(*job); };
    task.completion = [job]() { job->completed.store(true, std::memory_order_release); };
    if (!executor.post(std::move(task))) {
        return false;
    }
    job_ = std::move(job);
    return true;
}

void GlyphPrewarm::cancel() noexcept {
    if (job_ != nullptr) {
        job_->cancelled.store(true, std::memory_order_relaxed);
        job_.reset();
    }
}

bool GlyphPrewarm::pending() const noexcept {
    return job_ != nullptr;
}

bool GlyphPrewarm::ready() const noexcept {
    return job_ != nullptr && job_->completed.load(std::memory_order_acquire) &&
           job_->finished.load(std::memory_order_acquire);
}

std::size_t GlyphPrewarm::install(GlyphAtlas& atlas) {
    if (!ready()) {
        return 0;
    }
    std::size_t cached = 0;
    for (std::size_t i = 0; i < job_->keys.size(); ++i) {
        if (job_->rasterized[i] != 0 &&
            atlas.insert_prerasterized(job_->keys[i], job_->bitmaps[i])) {
            ++cached;
        }
    }
    job_.reset();
    return cached;
}

// The worker half. Touches only the job it was handed and rasterizers it created itself.
void GlyphPrewarm::run(Job& job) {
    std::vector<std::optional<GlyphRasterizer>> workers(job.sources.size());
    std::vector<bool> failed(job.sources.size(), false);
    for (std::size_t i = 0; i < job.keys.size(); ++i) {
        if (job.cancelled.load(std::memory_order_relaxed)) {
            break;
        }
        const GlyphAtlasKey& key = job.keys[i];
        if (key.face >= workers.size() || failed[key.face]) {
            continue;
        }
        std::optional<GlyphRasterizer>& worker = workers[key.face];
        if (!worker.has_value()) {
            auto clone = job.sources[key.face].clone_independent();
            if (!clone) {
                failed[key.face] = true;
                continue;
            }
            worker.emplace(std::move(*clone));
        }
        job.bitmaps[i] = worker->rasterize_glyph(key.glyph_index, key.pixel_size, job.hinting,
                                                 key.x_subpixel, job.aa_mode);
        job.rasterized[i] = 1;
    }
    job.finished.store(true, std::memory_order_release);
}

} // namespace tessera::ui::text
