// SPDX-License-Identifier: MIT
//
// Fixed-capacity ring buffer for one chart series (issue #182). Normative contract:
// docs/specs/chart-series-v1.md.
//
// The buffer is sized once by the constructor and never grows: appends overwrite the oldest sample
// when full, reads are by logical index (0 = oldest), and iteration runs oldest -> newest. Every
// operation after construction is allocation-free; append/read/iterate/clear are noexcept. The type
// is header-only on purpose: it has no ABI surface and no library target, so a later chart node can
// use it without a build-system change.
//
// Not thread-safe: a chart series is updated on the UI thread (docs/specs/chart-series-v1.md
// section 7). Async producers publish through runtime::DataStore and the app appends on the UI
// thread.

#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace tessera::ui::chart {

// One chart point. `timestamp` is seconds on the series' own monotonic clock (not a wall-clock
// calendar time); it must be finite and strictly increasing within one ring. `value` is passed
// through unchanged; its interpretation belongs to the chart node.
struct ChartSample {
    double timestamp = 0.0;
    float value = 0.0f;

    [[nodiscard]] friend bool operator==(const ChartSample&, const ChartSample&) = default;
};

static_assert(
    std::is_trivially_copyable_v<ChartSample>,
    "ChartSample must stay a small trivially copyable value: the ring copies it on append "
    "and the per-frame draw path reads it without lifetime management");

// One series: fixed-capacity, overwrite-oldest sample storage.
//
// Capacity is at least 1 and fixed for the object's lifetime. `append` rejects a non-finite or
// non-increasing timestamp without touching the stored samples, so the samples always stay in
// strictly increasing timestamp order. When the ring is full the append overwrites the oldest
// sample (logical index 0), which becomes the newest; `size()` stays at capacity.
//
// The two `spans()` describe the same logical sequence as indexed reads, split at the physical
// wrap point, so a renderer can consume the series without a copy.
class ChartSeriesRing final {
public:
    using value_type = ChartSample;
    using size_type = std::size_t;

    // One contiguous physical run of samples. `data` points into the ring's storage owned by this
    // object; it is invalidated by append(), clear(), move and destruction.
    struct Span {
        const ChartSample* data = nullptr;
        size_type size = 0;
    };
    using SpanPair = std::array<Span, 2>;

    class const_iterator;

    // Allocates the fixed storage exactly once. Throws std::invalid_argument for capacity 0 and
    // std::bad_alloc when the allocation fails.
    explicit ChartSeriesRing(size_type capacity);

    ~ChartSeriesRing() = default;

    ChartSeriesRing(const ChartSeriesRing&) = delete;
    ChartSeriesRing& operator=(const ChartSeriesRing&) = delete;

    // A move steals the storage and leaves the source inert (capacity 0, size 0): it is still
    // destructible and move-assignable, but it accepts no append until a new ring is assigned.
    ChartSeriesRing(ChartSeriesRing&& other) noexcept;
    ChartSeriesRing& operator=(ChartSeriesRing&& other) noexcept;

    [[nodiscard]] size_type capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] size_type size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] bool full() const noexcept {
        return size_ == capacity_ && capacity_ != 0;
    }

    // Appends `sample`; overwrites the oldest sample when full. Returns false (and changes nothing)
    // for an inert moved-from ring, a non-finite timestamp, or a timestamp that is not strictly
    // greater than the current newest one. Never allocates.
    [[nodiscard]] bool append(ChartSample sample) noexcept;

    // Logical read: 0 is the oldest sample, size() - 1 the newest. The index must be < size(); the
    // checked form only exists in debug builds (assert), the Release contract is a precondition.
    [[nodiscard]] const ChartSample& operator[](size_type index) const noexcept;
    [[nodiscard]] const ChartSample& oldest() const noexcept;
    [[nodiscard]] const ChartSample& newest() const noexcept;

    // Drops every sample (size becomes 0) but keeps the capacity and the storage. No allocation,
    // no zero-fill; the next append may carry any finite timestamp because the series starts a new
    // time range.
    void clear() noexcept;

    // The logical sequence as up to two contiguous spans: the first holds the samples from the
    // oldest to the physical end of the storage, the second the remainder from the storage start.
    // Concatenating them yields oldest -> newest. A non-wrapped ring returns one span and one
    // empty span; an empty ring returns two empty spans. No allocation.
    [[nodiscard]] SpanPair spans() const noexcept;

    // Forward iteration oldest -> newest. The iterator is read-only and invalidated by append(),
    // clear() and move.
    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;

private:
    [[nodiscard]] size_type physical(size_type logical) const noexcept {
        return (head_ + logical) % capacity_;
    }

    std::unique_ptr<ChartSample[]> storage_;
    size_type capacity_ = 0;
    size_type head_ = 0;
    size_type size_ = 0;
};

class ChartSeriesRing::const_iterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = ChartSample;
    using difference_type = std::ptrdiff_t;
    using pointer = const ChartSample*;
    using reference = const ChartSample&;

    const_iterator() noexcept = default;

    [[nodiscard]] reference operator*() const noexcept {
        return (*ring_)[index_];
    }
    [[nodiscard]] pointer operator->() const noexcept {
        return &(*ring_)[index_];
    }

    const_iterator& operator++() noexcept {
        ++index_;
        return *this;
    }
    const_iterator operator++(int) noexcept {
        const_iterator copy = *this;
        ++index_;
        return copy;
    }

    [[nodiscard]] friend bool operator==(const const_iterator&,
                                         const const_iterator&) noexcept = default;

private:
    friend class ChartSeriesRing;

    const_iterator(const ChartSeriesRing* ring, size_type index) noexcept
        : ring_(ring),
          index_(index) {}

    const ChartSeriesRing* ring_ = nullptr;
    size_type index_ = 0;
};

inline ChartSeriesRing::ChartSeriesRing(size_type capacity)
    : storage_(capacity == 0 ? nullptr : std::make_unique<ChartSample[]>(capacity)),
      capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("chart series ring capacity must be at least 1");
    }
}

inline ChartSeriesRing::ChartSeriesRing(ChartSeriesRing&& other) noexcept
    : storage_(std::move(other.storage_)),
      capacity_(other.capacity_),
      head_(other.head_),
      size_(other.size_) {
    other.capacity_ = 0;
    other.head_ = 0;
    other.size_ = 0;
}

inline ChartSeriesRing& ChartSeriesRing::operator=(ChartSeriesRing&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        capacity_ = other.capacity_;
        head_ = other.head_;
        size_ = other.size_;
        other.capacity_ = 0;
        other.head_ = 0;
        other.size_ = 0;
    }
    return *this;
}

inline bool ChartSeriesRing::append(ChartSample sample) noexcept {
    if (capacity_ == 0 || !std::isfinite(sample.timestamp)) {
        return false;
    }
    if (size_ != 0 && !(sample.timestamp > storage_[physical(size_ - 1)].timestamp)) {
        return false;
    }
    if (size_ < capacity_) {
        storage_[physical(size_)] = sample;
        ++size_;
        return true;
    }
    storage_[head_] = sample;
    head_ = (head_ + 1) % capacity_;
    return true;
}

inline const ChartSample& ChartSeriesRing::operator[](size_type index) const noexcept {
    assert(index < size_ && "chart series ring index out of range");
    return storage_[physical(index)];
}

inline const ChartSample& ChartSeriesRing::oldest() const noexcept {
    assert(size_ != 0 && "oldest() on an empty chart series ring");
    return storage_[head_];
}

inline const ChartSample& ChartSeriesRing::newest() const noexcept {
    assert(size_ != 0 && "newest() on an empty chart series ring");
    return storage_[physical(size_ - 1)];
}

inline void ChartSeriesRing::clear() noexcept {
    head_ = 0;
    size_ = 0;
}

inline ChartSeriesRing::SpanPair ChartSeriesRing::spans() const noexcept {
    if (size_ == 0) {
        return SpanPair{};
    }
    const size_type first = capacity_ - head_ < size_ ? capacity_ - head_ : size_;
    return SpanPair{Span{storage_.get() + head_, first}, Span{storage_.get(), size_ - first}};
}

inline ChartSeriesRing::const_iterator ChartSeriesRing::begin() const noexcept {
    return const_iterator(this, 0);
}

inline ChartSeriesRing::const_iterator ChartSeriesRing::end() const noexcept {
    return const_iterator(this, size_);
}

} // namespace tessera::ui::chart
