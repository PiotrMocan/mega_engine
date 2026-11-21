#pragma once

#include <cstddef>
#include <functional>
#include <limits>

namespace vcr {

// Synchronous indexed jobs on the shared Windows thread pool. The caller
// participates and all callbacks finish before captures may go out of scope.
void parallel_for_jobs(std::size_t count, const std::function<void(std::size_t)>& job,
                       std::size_t concurrency_limit = std::numeric_limits<std::size_t>::max());

} // namespace vcr
