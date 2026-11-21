#pragma once

#include <glm/vec3.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace vcr {

// UI-owned. Dots acknowledge recent picks until the corresponding model is visible.
template<class Point>
class StudioBrushPreviewTrail final {
public:
    using Clock = std::chrono::steady_clock;

    void record(const Point& position, Clock::time_point now = Clock::now()) noexcept {
        if (!dirty()) first_edit_ = now;
        points_[edited_ % points_.size()] = position;
        ++edited_;
    }
    bool dirty() const noexcept { return submitted_ != edited_; }
    bool due(bool held, Clock::time_point now = Clock::now()) const noexcept {
        return dirty() && (!held || now - first_edit_ >= std::chrono::milliseconds(75));
    }
    void submitted() noexcept { submitted_ = edited_; }
    void published() noexcept { displayed_ = submitted_; }
    void reset() noexcept { displayed_ = submitted_ = edited_ = 0; }

    template<class Draw> void draw_pending(Draw draw) const {
        const auto count = std::min<std::uint64_t>(edited_ - displayed_, points_.size());
        for (auto i = edited_ - count; i < edited_; ++i) draw(points_[i % points_.size()]);
    }

private:
    std::array<Point, 64> points_{};
    std::uint64_t edited_ = 0, submitted_ = 0, displayed_ = 0;
    Clock::time_point first_edit_{};
};

using StudioBrushPreview = StudioBrushPreviewTrail<glm::vec3>;

} // namespace vcr
