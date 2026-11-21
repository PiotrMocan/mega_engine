#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace vcr {

// One CPU bake per studio. The UI keeps the latest edited document until the
// worker is free, so a long gesture never queues copies of intermediate dabs.
template<class Input, class Output>
class StudioPreviewWorker final {
public:
    using Bake = void (*)(const Input&, Output&, std::stop_token);
    explicit StudioPreviewWorker(Bake bake) noexcept : bake_(bake) {}
    ~StudioPreviewWorker() { stop(); }

    [[nodiscard]] bool busy() const noexcept { return busy_; } // UI thread only
    [[nodiscard]] bool pending() const noexcept { return busy_ && !discard_; }
    void invalidate() noexcept {
        discard_ = true;
        job_stop_.request_stop();
    }

    void submit(const Input& input) {
        assert(!busy_);
        if (!thread_.joinable())
            thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
        {
            std::lock_guard lock(mutex_);
            job_stop_ = std::stop_source{};
            input_.emplace(input);
        }
        busy_ = true;
        discard_ = false;
        wake_.notify_one();
    }

    [[nodiscard]] std::optional<Output> take() {
        if (!busy_ || !ready_.load(std::memory_order_acquire)) return {};
        std::optional<Output> result;
        if (!discard_) result.emplace(std::move(*output_));
        output_.reset();
        ready_.store(false, std::memory_order_relaxed);
        busy_ = false;
        return result;
    }

    void stop() noexcept {
        if (!thread_.joinable()) return;
        invalidate();
        thread_.request_stop();
        thread_.join();
        input_.reset();
        output_.reset();
        busy_ = false;
        ready_.store(false, std::memory_order_relaxed);
    }

private:
    void run(std::stop_token stop) {
        for (;;) {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] { return input_.has_value(); });
            if (stop.stop_requested()) return;
            Input input = std::move(*input_);
            auto job_stop = job_stop_.get_token();
            input_.reset();
            lock.unlock();
            auto& result = output_.emplace();
            try {
                bake_(input, result, job_stop);
            } catch (const std::exception& e) {
                result.error = e.what();
            }
            // Only take() consumes the result. A subsequent submission hands
            // this slot back to the worker through mutex_.
            ready_.store(true, std::memory_order_release);
        }
    }

    Bake bake_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Input> input_;
    std::optional<Output> output_;
    std::atomic<bool> ready_{false};
    bool busy_ = false;
    bool discard_ = false;
    std::stop_source job_stop_{std::nostopstate};
    std::jthread thread_;
};

} // namespace vcr
