#include "job_system.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <thread>

namespace vcr {
namespace {
struct Batch {
    const std::function<void(std::size_t)>& job;
    std::size_t count;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr failure{};

    void drain() noexcept {
        try {
            while (!failed.load(std::memory_order_relaxed)) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count)
                    break;
                job(index);
            }
        } catch (...) {
            // Only the winner writes. The owner reads after joining the batch.
            if (!failed.exchange(true, std::memory_order_relaxed))
                failure = std::current_exception();
        }
    }
};

void CALLBACK run_batch(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
    static_cast<Batch*>(context)->drain();
}
} // namespace

void parallel_for_jobs(const std::size_t count, const std::function<void(std::size_t)>& job,
                       const std::size_t concurrency_limit) {
    const unsigned cores = std::thread::hardware_concurrency();
    const auto lanes = std::min({count, concurrency_limit, std::size_t(cores > 1 ? cores - 1 : 1)});
    if (lanes <= 1) {
        for (std::size_t index = 0; index < count; ++index)
            job(index);
        return;
    }
    Batch batch{job, count};
    const std::unique_ptr<TP_WORK, decltype(&CloseThreadpoolWork)> work(
        CreateThreadpoolWork(run_batch, &batch, nullptr), CloseThreadpoolWork);
    if (work) {
        for (std::size_t lane = 1; lane < lanes; ++lane)
            SubmitThreadpoolWork(work.get());
    }
    batch.drain();
    if (work)
        WaitForThreadpoolWorkCallbacks(work.get(), TRUE);
    if (batch.failure)
        std::rethrow_exception(batch.failure);
}
} // namespace vcr
