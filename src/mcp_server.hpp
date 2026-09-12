#pragma once

#include "mcp_control.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace vcr {

enum class McpServerProfile : std::uint8_t {
    runtime,
    editor,
};

class McpServer final {
public:
    explicit McpServer(
        McpControlPlane& control,
        const McpServerProfile profile = McpServerProfile::runtime) noexcept
        : control_(control), profile_(profile) {}
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    void run(std::stop_token stop_token) noexcept;

    std::reference_wrapper<McpControlPlane> control_;
    McpServerProfile profile_ = McpServerProfile::runtime;
    std::jthread thread_{};
    std::atomic<bool> running_{false};
};

} // namespace vcr
