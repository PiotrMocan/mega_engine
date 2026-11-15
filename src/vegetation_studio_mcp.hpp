#pragma once

#include "mcp_control.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcr {

struct VegMcpCommand final {
    std::uint64_t request_id = 0U;
    std::string tool_name{};
    nlohmann::json arguments = nlohmann::json::object();
};

struct VegMcpResponse final {
    std::uint64_t request_id = 0U;
    bool ok = false;
    std::string error{};
    nlohmann::json payload = nlohmann::json::object();
    std::vector<std::uint8_t> screenshot_png{};
    std::uint32_t screenshot_width = 0U;
    std::uint32_t screenshot_height = 0U;
};

struct VegetationStudioMcpControl final {
    SpscQueue<VegMcpCommand, 64U> requests{};
    SpscQueue<VegMcpResponse, 64U> responses{};
    std::atomic<bool> quit_requested{false};
    std::mutex response_mutex{};
    std::condition_variable response_cv{};

    void notify_response_available() noexcept {
        response_cv.notify_all();
    }
};

/// Dedicated JSON-RPC over stdio server (`2025-11-25`) exposing procedural
/// vegetation generation, 3D crown painting, LOD/benchmark arenas, and project export.
class VegetationStudioMcpServer final {
public:
    explicit VegetationStudioMcpServer(VegetationStudioMcpControl& control) noexcept;
    ~VegetationStudioMcpServer();

    VegetationStudioMcpServer(const VegetationStudioMcpServer&) = delete;
    VegetationStudioMcpServer& operator=(const VegetationStudioMcpServer&) = delete;

    void start();
    void stop() noexcept;

private:
    void run(std::stop_token stop_token);
    void handle_line(const std::string& line);
    void handle_tools_call(const nlohmann::json& id, const nlohmann::json& params);
    [[nodiscard]] bool dispatch_and_wait(
        std::string_view tool_name,
        const nlohmann::json& arguments,
        VegMcpResponse& out_response);

    VegetationStudioMcpControl& control_;
    std::jthread worker_{};
    std::atomic<std::uint64_t> next_request_id_{1U};
    bool initialized_ = false;
};

} // namespace vcr
