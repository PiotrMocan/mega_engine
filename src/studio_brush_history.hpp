#pragma once

#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

namespace vcr {

// Only the edited brush layer is stored. A drag takes one snapshot at its
// first accepted dab; meshes, atlases and other document settings stay live.
template<class Sample>
class StudioBrushHistory {
public:
    using Layer = std::vector<Sample>;
    static constexpr std::size_t max_steps = 32;

    [[nodiscard]] bool append(Layer& layer, const Sample& sample, std::size_t limit, bool continuing) {
        if (layer.size() >= limit) return false;
        if (!continuing || !drawing_) {
            push(undo_, layer);
            redo_.clear();
        }
        layer.push_back(sample);
        drawing_ = true;
        return true;
    }

    [[nodiscard]] bool clear(Layer& layer) {
        if (layer.empty()) return false;
        push(undo_, std::move(layer));
        layer.clear();
        redo_.clear();
        drawing_ = false;
        return true;
    }

    [[nodiscard]] bool can_undo(const Layer& layer) const noexcept {
        return !undo_.empty() || !layer.empty();
    }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] bool undo(Layer& layer) {
        if (!can_undo(layer)) return false;
        if (undo_.empty()) {
            // Existing recipes store sample positions but have no gesture boundaries.
            push(redo_, layer);
            layer.pop_back();
        } else {
            push(redo_, std::move(layer));
            layer = std::move(undo_.back());
            undo_.pop_back();
        }
        drawing_ = false;
        return true;
    }

    [[nodiscard]] bool redo(Layer& layer) {
        if (redo_.empty()) return false;
        push(undo_, std::move(layer));
        layer = std::move(redo_.back());
        redo_.pop_back();
        drawing_ = false;
        return true;
    }

    void reset() noexcept {
        undo_.clear();
        redo_.clear();
        drawing_ = false;
    }

private:
    template<class State>
    static void push(std::deque<Layer>& stack, State&& state) {
        stack.emplace_back(std::forward<State>(state));
        if (stack.size() > max_steps) stack.pop_front();
    }

    std::deque<Layer> undo_, redo_;
    bool drawing_ = false;
};

} // namespace vcr
