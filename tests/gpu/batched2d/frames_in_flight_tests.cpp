// SPDX-License-Identifier: MIT
//
// Vertex memory handling of Batched2DRenderer, observed through a recording decorator around the
// null device (deterministic, no GPU):
//  - flushes within one frame append instead of overwriting offset 0, because draws recorded by an
//    earlier flush have not executed yet;
//  - growth inside a frame never destroys a buffer that recorded draws still reference;
//  - frames_in_flight slots are used round-robin, so frame k + 1 never writes frame k's buffer.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace tessera::rendering;

struct WriteRecord {
    int buffer = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct DrawRecord {
    int buffer = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
};

struct Log {
    int next_buffer_id = 0;
    int destroyed_buffers = 0;
    std::vector<WriteRecord> writes;
    std::vector<DrawRecord> draws;
};

class RecordingBuffer final : public rhi::Buffer {
public:
    RecordingBuffer(std::unique_ptr<rhi::Buffer> inner, Log& log)
        : inner_(std::move(inner)),
          log_(log),
          id_(log.next_buffer_id++) {}
    ~RecordingBuffer() override {
        ++log_.destroyed_buffers;
    }

    [[nodiscard]] std::size_t size() const override {
        return inner_->size();
    }
    void write(const void* data, std::size_t size, std::size_t offset) override {
        log_.writes.push_back({id_, offset, size});
        inner_->write(data, size, offset);
    }
    void read(void* data, std::size_t size, std::size_t offset) const override {
        inner_->read(data, size, offset);
    }
    [[nodiscard]] int id() const {
        return id_;
    }

private:
    std::unique_ptr<rhi::Buffer> inner_;
    Log& log_;
    int id_ = 0;
};

class RecordingCommandList final : public rhi::CommandList {
public:
    RecordingCommandList(std::unique_ptr<rhi::CommandList> inner, Log& log)
        : inner_(std::move(inner)),
          log_(log) {}

    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override {
        inner_->begin_render_pass(target, desc);
    }
    void end_render_pass() override {
        inner_->end_render_pass();
    }
    void bind_pipeline(const rhi::Pipeline& pipeline) override {
        inner_->bind_pipeline(pipeline);
    }
    void bind_vertex_buffer(const rhi::Buffer& buffer) override {
        bound_ = static_cast<const RecordingBuffer&>(buffer).id();
        inner_->bind_vertex_buffer(buffer);
    }
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override {
        inner_->bind_texture(slot, texture);
    }
    void set_viewport(const rhi::Viewport& viewport) override {
        inner_->set_viewport(viewport);
    }
    void set_scissor(const rhi::Scissor& scissor) override {
        inner_->set_scissor(scissor);
    }
    void push_constants(const void* data, std::size_t size) override {
        inner_->push_constants(data, size);
    }
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex) override {
        log_.draws.push_back({bound_, first_vertex, vertex_count});
        inner_->draw(vertex_count, first_vertex);
    }
    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override {
        inner_->copy_target_to_buffer(target, buffer);
    }
    void submit() override {
        inner_->submit();
    }
    void submit_async() override {
        inner_->submit_async();
    }
    [[nodiscard]] bool is_complete() const override {
        return inner_->is_complete();
    }
    [[nodiscard]] bool wait(std::uint64_t timeout_ms) override {
        return inner_->wait(timeout_ms);
    }

private:
    std::unique_ptr<rhi::CommandList> inner_;
    Log& log_;
    int bound_ = -1;
};

class RecordingDevice final : public rhi::Device {
public:
    explicit RecordingDevice(Log& log)
        : inner_(null::create_null_device(rhi::DeviceDesc{})),
          log_(log) {}

    [[nodiscard]] rhi::Backend backend() const override {
        return inner_->backend();
    }
    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        return std::make_unique<RecordingBuffer>(inner_->create_buffer(desc), log_);
    }
    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        return inner_->create_texture(desc);
    }
    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override {
        return inner_->create_pipeline(desc);
    }
    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override {
        return inner_->create_render_target(desc);
    }
    std::unique_ptr<rhi::CommandList> create_command_list() override {
        return std::make_unique<RecordingCommandList>(inner_->create_command_list(), log_);
    }
    std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc& desc) override {
        return inner_->create_swapchain(desc);
    }
    void wait_idle() override {
        inner_->wait_idle();
    }

private:
    std::unique_ptr<rhi::Device> inner_;
    Log& log_;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frames_in_flight: " << message << "\n";
    }
    return condition;
}

constexpr std::uint32_t kStride = gpu::kVertex2DStride;
constexpr std::uint32_t kQuadVertices = 6;

struct Fixture {
    Log log;
    RecordingDevice device{log};
    std::unique_ptr<rhi::RenderTarget> target = device.create_render_target({{64, 64}});
    std::unique_ptr<rhi::Texture> white = device.create_texture({{1, 1}});
    std::unique_ptr<rhi::Pipeline> pipeline = device.create_pipeline({});
};

void push_rects(gpu::Batched2DRenderer& renderer, int count) {
    for (int i = 0; i < count; ++i) {
        renderer.push_rect({static_cast<float>(i), 0, 4, 4}, rhi::rgba8(200, 30, 30));
    }
}

// One frame with `rect_counts` rects per flush; returns once the frame ended.
void run_frame(Fixture& fixture, gpu::Batched2DRenderer& renderer,
               const std::vector<int>& rect_counts) {
    auto commands = fixture.device.create_command_list();
    renderer.begin(*commands, *fixture.target, rhi::RenderTargetDesc{{64, 64}});
    for (std::size_t i = 0; i < rect_counts.size(); ++i) {
        push_rects(renderer, rect_counts[i]);
        if (i + 1 < rect_counts.size()) {
            renderer.flush();
        }
    }
    renderer.end();
    commands->submit();
}

bool check_flushes_append() {
    Fixture f;
    gpu::Batched2DRenderer renderer(f.device, *f.pipeline, *f.white);
    run_frame(f, renderer, {2, 3});

    return expect(f.log.writes.size() == 2, "two flushes write twice") &&
           expect(f.log.writes[0].offset == 0 &&
                      f.log.writes[0].size == 2 * kQuadVertices * kStride,
                  "first flush writes at offset 0") &&
           expect(f.log.writes[1].buffer == f.log.writes[0].buffer &&
                      f.log.writes[1].offset == 2 * kQuadVertices * kStride,
                  "second flush appends after the first instead of overwriting offset 0") &&
           expect(f.log.draws.size() == 2 && f.log.draws[0].first_vertex == 0 &&
                      f.log.draws[1].first_vertex == 2 * kQuadVertices &&
                      f.log.draws[1].vertex_count == 3 * kQuadVertices,
                  "draws index the appended ranges");
}

bool check_single_slot_reuses_buffer() {
    Fixture f;
    gpu::Batched2DRenderer renderer(f.device, *f.pipeline, *f.white);
    run_frame(f, renderer, {1});
    run_frame(f, renderer, {1});
    return expect(f.log.draws.size() == 2 && f.log.draws[0].buffer == f.log.draws[1].buffer,
                  "frames_in_flight = 1 reuses the same buffer") &&
           expect(f.log.writes[1].offset == 0, "a new frame starts at offset 0");
}

bool check_slots_rotate() {
    Fixture f;
    gpu::Batched2DConfig config;
    config.frames_in_flight = 2;
    gpu::Batched2DRenderer renderer(f.device, *f.pipeline, *f.white, config);
    for (int frame = 0; frame < 4; ++frame) {
        run_frame(f, renderer, {1});
    }
    const auto& d = f.log.draws;
    return expect(d.size() == 4, "four frames drew") &&
           expect(d[0].buffer != d[1].buffer, "adjacent frames use different buffers") &&
           expect(d[0].buffer == d[2].buffer && d[1].buffer == d[3].buffer,
                  "slots repeat every frames_in_flight frames") &&
           expect(f.log.writes[2].offset == 0, "a reused slot restarts at offset 0");
}

bool check_growth_keeps_recorded_buffers_alive() {
    Fixture f;
    gpu::Batched2DRenderer renderer(f.device, *f.pipeline, *f.white,
                                    gpu::Batched2DConfig{64, true, false});
    const int destroyed_before = f.log.destroyed_buffers;

    auto commands = f.device.create_command_list();
    renderer.begin(*commands, *f.target, rhi::RenderTargetDesc{{64, 64}});
    push_rects(renderer, 7); // 42 vertices fit in the 64-vertex buffer
    renderer.flush();
    push_rects(renderer, 5); // 42 + 30 > 64: continues in a larger buffer
    renderer.end();
    const int destroyed_in_frame = f.log.destroyed_buffers - destroyed_before;
    commands->submit();

    if (!expect(f.log.draws.size() == 2, "both flushes drew") ||
        !expect(f.log.draws[0].buffer != f.log.draws[1].buffer, "growth switched buffers") ||
        !expect(f.log.draws[1].first_vertex == 0, "the new buffer starts at vertex 0") ||
        !expect(destroyed_in_frame == 0,
                "no buffer referenced by recorded draws is destroyed during the frame")) {
        return false;
    }

    // The next frame reuses the slot, so the growth buffer is dropped and the larger one kept.
    run_frame(f, renderer, {1});
    return expect(f.log.destroyed_buffers - destroyed_before == 1,
                  "reusing the slot drops only the superseded buffer") &&
           expect(f.log.draws.back().buffer == f.log.draws[1].buffer,
                  "the larger buffer is kept for later frames");
}

} // namespace

int main() {
    if (!check_flushes_append()) {
        return 1;
    }
    if (!check_single_slot_reuses_buffer()) {
        return 2;
    }
    if (!check_slots_rotate()) {
        return 3;
    }
    if (!check_growth_keeps_recorded_buffers_alive()) {
        return 4;
    }
    std::cout << "frames_in_flight: ok\n";
    return 0;
}
