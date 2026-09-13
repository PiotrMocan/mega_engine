#include "device_object.hpp"
#include "gpu_buffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

struct SdlLifetime {
    SdlLifetime() { if (!SDL_Init(SDL_INIT_VIDEO)) vcr::fatal(SDL_GetError()); }
    ~SdlLifetime() { SDL_Quit(); }
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept { SDL_DestroyWindow(window); }
};

struct SubmissionRetirement {
    VkDevice device; // Observer; context and submitted resources outlive this guard.
    bool pending = false;
    ~SubmissionRetirement() {
        if (pending) {
            const auto result = vkDeviceWaitIdle(device);
            if (result != VK_SUCCESS)
                std::cerr << "Exposure test retirement failed: VkResult " << result << '\n';
        }
    }
};

constexpr std::size_t sample_capacity = 16;
constexpr std::size_t case_count = 8;
using Float4 = std::array<float, 4>;
using Uint4 = std::array<std::uint32_t, 4>;

struct alignas(16) ExposureCase {
    std::array<Float4, sample_capacity> samples{};
    Uint4 control{};
    Float4 previous{};
    Float4 settings0{};
    Float4 settings1{};
};

struct alignas(16) ExposureResult {
    std::array<std::uint32_t, 256> bins{};
    Uint4 counters{};
    Float4 values{};
};

static_assert(sizeof(ExposureCase) == 320 && offsetof(ExposureCase, control) == 256 &&
              offsetof(ExposureCase, previous) == 272 && offsetof(ExposureCase, settings0) == 288 &&
              offsetof(ExposureCase, settings1) == 304);
static_assert(sizeof(ExposureResult) == 1056 && offsetof(ExposureResult, counters) == 1024 &&
              offsetof(ExposureResult, values) == 1040);
static_assert(std::is_trivially_copyable_v<ExposureCase> &&
              std::is_trivially_copyable_v<ExposureResult>);

ExposureCase make_case(std::span<const Float4> samples, float delta_seconds = 0.0F,
                       float previous_exposure = 1.0F, bool history_valid = false,
                       bool reset = false, float low_percentile = 0.02F,
                       float high_percentile = 0.98F) {
    if (samples.size() > sample_capacity) vcr::fatal("Exposure GPU fixture exceeds sample capacity");
    ExposureCase result{};
    std::copy(samples.begin(), samples.end(), result.samples.begin());
    result.control = {static_cast<std::uint32_t>(samples.size()), reset ? 1U : 0U,
                      history_valid ? 1U : 0U, 0U};
    result.previous = {previous_exposure, previous_exposure, 0.18F, 0.0F};
    result.settings0 = {delta_seconds, 0.0F, -10.0F, 10.0F};
    result.settings1 = {low_percentile, high_percentile, 1.0F, 0.25F};
    return result;
}

Float4 grey(const float value) noexcept { return {value, value, value, 1.0F}; }

std::array<ExposureCase, case_count> make_cases() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array bin_mapping{
        grey(0.0F), grey(-2.0F), grey(std::exp2(-17.0F)), grey(std::exp2(-15.5F)),
        grey(std::exp2(-8.25F)), grey(0.18F), grey(std::exp2(0.25F)), grey(std::exp2(8.25F)),
        grey(std::exp2(15.5F)), grey(std::exp2(17.0F)), Float4{nan, 1.0F, 1.0F, 1.0F},
        Float4{1.0F, infinity, 1.0F, 1.0F}, Float4{4.0F, 0.0F, 0.0F, 1.0F},
        Float4{0.0F, 1.0F, 0.0F, 1.0F}, Float4{0.0F, 0.0F, 1.0F, 1.0F}};
    const std::array percentile_samples{
        grey(0.0F), grey(0.0F), grey(std::exp2(-8.0F)), grey(std::exp2(-8.0F)),
        grey(std::exp2(-2.0F)), grey(std::exp2(-2.0F)), grey(std::exp2(0.25F)),
        grey(std::exp2(0.25F)), grey(std::exp2(0.25F)), grey(std::exp2(0.25F)),
        grey(std::exp2(4.0F)), grey(std::exp2(4.0F)),
        grey(std::exp2(10.0F)), grey(std::exp2(10.0F)),
        Float4{infinity, 0.0F, 0.0F, 1.0F}, Float4{0.0F, nan, 0.0F, 1.0F}};
    const std::array black_samples{grey(0.0F), grey(-1.0F), grey(-100.0F)};
    const std::array invalid_samples{
        Float4{nan, 0.0F, 0.0F, 1.0F}, Float4{0.0F, infinity, 0.0F, 1.0F}};
    const std::array middle_grey{grey(0.18F), grey(0.18F), grey(0.18F), grey(0.18F)};
    const std::array dark_samples{grey(0.045F), grey(0.045F), grey(0.045F), grey(0.045F)};
    const std::array bright_samples{grey(0.72F), grey(0.72F), grey(0.72F), grey(0.72F)};
    const std::array mixed_samples{
        grey(0.03F), grey(0.06F), grey(0.12F), grey(0.24F),
        grey(0.48F), grey(0.96F), grey(1.92F), grey(3.84F)};

    std::array<ExposureCase, case_count> result{
        make_case(bin_mapping),
        make_case(percentile_samples, 0.0F, 1.0F, false, false, 0.20F, 0.80F),
        make_case(black_samples, 0.25F, 3.0F, true),
        make_case(invalid_samples, 0.25F, nan, false),
        make_case(middle_grey, 0.5F, 8.0F, true, true),
        make_case(dark_samples, 0.25F, 0.25F, true),
        make_case(bright_samples, 0.25F, 4.0F, true),
        make_case(mixed_samples, 2.0F, 0.5F, true),
    };
    result[2].previous = {3.0F, 4.0F, 0.5F, 0.0F};
    result[3].previous = {nan, 7.0F, 2.0F, 0.0F};
    return result;
}

std::uint32_t reference_bin(const Float4& source, bool& valid) noexcept {
    valid = std::isfinite(source[0]) && std::isfinite(source[1]) && std::isfinite(source[2]);
    if (!valid) return 0;
    const double luminance = std::max(0.0, static_cast<double>(source[0])) * 0.2126 +
        std::max(0.0, static_cast<double>(source[1])) * 0.7152 +
        std::max(0.0, static_cast<double>(source[2])) * 0.0722;
    if (!(luminance > 0.0)) return 0;
    const double log_luminance = std::log2(luminance);
    if (log_luminance >= 16.0) return 255;
    if (log_luminance < -16.0) return 0;
    const double mapped = std::floor((log_luminance + 16.0) * (254.0 / 32.0));
    return 1U + static_cast<std::uint32_t>(std::clamp(mapped, 0.0, 253.0));
}

ExposureResult reference_result(const ExposureCase& input) {
    ExposureResult result{};
    const auto sample_count = std::min(
        input.control[0], static_cast<std::uint32_t>(sample_capacity));
    for (std::uint32_t index = 0; index < sample_count; ++index) {
        bool valid = false;
        const auto bin = reference_bin(input.samples[index], valid);
        if (valid) ++result.bins[bin];
        else ++result.counters[0];
    }

    std::uint32_t total = 0;
    for (const auto count : result.bins) total += count;
    double average_log_luminance = std::log2(0.18);
    if (total != 0) {
        const double low = std::floor(static_cast<double>(total) * input.settings1[0]);
        const double high = std::ceil(static_cast<double>(total) * input.settings1[1]);
        double previous_count = result.bins[0];
        double weighted_log_sum = 0.0;
        double included_weight = 0.0;
        for (std::uint32_t bin = 1; bin < 256; ++bin) {
            const double current_count = previous_count + result.bins[bin];
            const double selected = std::max(
                0.0, std::min(current_count, high) - std::max(previous_count, low));
            if (selected > 0.0) {
                const double center = bin == 255 ? 16.0 :
                    -16.0 + (static_cast<double>(bin) - 0.5) * (32.0 / 254.0);
                weighted_log_sum += center * selected;
                included_weight += selected;
            }
            previous_count = current_count;
        }
        if (included_weight >= std::max(1.0, std::ceil(static_cast<double>(total) * 0.01)))
            average_log_luminance = weighted_log_sum / included_weight;
        else
            total = 0;
    }

    bool history_valid = input.control[2] != 0;
    if (input.control[1] != 0) history_valid = false;
    double exposure = input.previous[0];
    double target = input.previous[1];
    double average_luminance = input.previous[2];
    if (total == 0) {
        if (!history_valid || !std::isfinite(exposure) || exposure <= 0.0) {
            exposure = 1.0;
            target = 1.0;
            average_luminance = 0.18;
        }
        result.counters[1] = 0;
        result.counters[2] = history_valid ? 1U : 0U;
        result.values = {static_cast<float>(exposure), static_cast<float>(target),
                         static_cast<float>(average_luminance), 0.0F};
        return result;
    }

    const double target_ev = std::clamp(
        std::log2(0.18) - average_log_luminance + input.settings0[1],
        static_cast<double>(input.settings0[2]), static_cast<double>(input.settings0[3]));
    target = std::exp2(target_ev);
    double adapted = target;
    if (history_valid && std::isfinite(exposure) && exposure > 0.0) {
        const double current_ev = std::log2(exposure);
        const double seconds = target_ev > current_ev ? input.settings1[2] : input.settings1[3];
        const double dt = std::clamp(static_cast<double>(input.settings0[0]), 0.0, 1.0);
        const double alpha = 1.0 - std::exp(-dt / seconds);
        adapted = std::exp2(std::lerp(current_ev, target_ev, alpha));
    }
    result.counters[1] = total;
    result.counters[2] = 1;
    result.values = {
        static_cast<float>(std::clamp(adapted, std::exp2(-24.0), std::exp2(24.0))),
        static_cast<float>(target), static_cast<float>(std::exp2(average_log_luminance)), 0.0F};
    return result;
}

bool close_float(const float actual, const float expected, double& maximum_error) noexcept {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return actual == expected;
    const double error = std::abs(static_cast<double>(actual) - expected);
    const double tolerance = 3.0e-5 * std::max(1.0, std::abs(static_cast<double>(expected)));
    maximum_error = std::max(maximum_error, error);
    return error <= tolerance;
}

bool verify_results(std::span<const ExposureCase> inputs,
                    std::span<const ExposureResult> actual) {
    bool passed = true;
    double maximum_error = 0.0;
    for (std::size_t case_index = 0; case_index < inputs.size(); ++case_index) {
        const auto expected = reference_result(inputs[case_index]);
        for (std::size_t bin = 0; bin < expected.bins.size(); ++bin) {
            if (actual[case_index].bins[bin] != expected.bins[bin]) {
                std::cerr << "Exposure histogram mismatch case=" << case_index << " bin=" << bin
                          << " expected=" << expected.bins[bin]
                          << " actual=" << actual[case_index].bins[bin] << '\n';
                passed = false;
            }
        }
        for (std::size_t counter = 0; counter < expected.counters.size(); ++counter) {
            if (actual[case_index].counters[counter] != expected.counters[counter]) {
                std::cerr << "Exposure counter mismatch case=" << case_index
                          << " field=" << counter << " expected=" << expected.counters[counter]
                          << " actual=" << actual[case_index].counters[counter] << '\n';
                passed = false;
            }
        }
        for (std::size_t value = 0; value < expected.values.size(); ++value) {
            if (!close_float(actual[case_index].values[value], expected.values[value], maximum_error)) {
                std::cerr << std::scientific << std::setprecision(9)
                          << "Exposure value mismatch case=" << case_index << " field=" << value
                          << " expected=" << expected.values[value]
                          << " actual=" << actual[case_index].values[value] << '\n';
                passed = false;
            }
        }
    }
    if (passed) {
        std::cout << std::scientific << std::setprecision(9)
                  << "Exposure GPU reference: " << inputs.size()
                  << " cases passed; all 256-bin histograms exact; max numeric error="
                  << maximum_error << '\n';
    }
    return passed;
}

std::vector<std::uint32_t> load_shader() {
    std::ifstream file(VCR_EXPOSURE_TEST_SPV, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (!file || length <= 0 || length % 4 != 0)
        vcr::fatal("Missing exposure reference SPIR-V; compile shaders first");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), length))
        vcr::fatal("Cannot read exposure reference SPIR-V");
    return code;
}

int run() {
    const auto inputs = make_cases();
    std::array<ExposureResult, case_count> actual{};

    SdlLifetime sdl;
    std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
        "Exposure numerical reference", 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
    if (!window) vcr::fatal(SDL_GetError());
    vcr::VulkanContext context;
    context.initialize(window.get(), true);
    if (!context.validation_enabled()) vcr::fatal("Exposure GPU test requires validation layer");
    const auto device = context.device();
    const auto& limits = context.properties().limits;
    if (limits.maxComputeWorkGroupCount[0] < static_cast<std::uint32_t>(case_count) ||
        limits.maxStorageBufferRange < sizeof(inputs) ||
        limits.maxStorageBufferRange < sizeof(actual))
        vcr::fatal("Exposure GPU test exceeds device compute/storage limits");
    const auto name = [&](VkObjectType type, auto handle, const char* label) {
        context.set_debug_name(type, vcr::handle_value(handle), label);
    };

    vcr::GpuBuffer input_buffer, output_buffer;
    input_buffer.create(context, sizeof(inputs), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT, "exposure reference inputs");
    output_buffer.create(context, sizeof(actual), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT, "exposure reference outputs");
    input_buffer.write(std::as_bytes(std::span(inputs)));

    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()), .pBindings = bindings.data()};
    vcr::DeviceObject<VkDescriptorSetLayout> layout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
        layout.put(device, vkDestroyDescriptorSetLayout)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, layout.get(), "exposure reference descriptor layout");
    const auto layout_handle = layout.get();

    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    vcr::DeviceObject<VkDescriptorPool> pool;
    VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr,
        pool.put(device, vkDestroyDescriptorPool)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_POOL, pool.get(), "exposure reference descriptor pool");
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool.get(), .descriptorSetCount = 1, .pSetLayouts = &layout_handle};
    VkDescriptorSet descriptor_set{}; // Descriptor pool owns this handle.
    VK_CHECK(vkAllocateDescriptorSets(device, &allocate, &descriptor_set));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET, descriptor_set, "exposure reference descriptors");
    const std::array buffer_infos{
        VkDescriptorBufferInfo{input_buffer.handle(), 0, input_buffer.size()},
        VkDescriptorBufferInfo{output_buffer.handle(), 0, output_buffer.size()}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t binding = 0;
         binding < static_cast<std::uint32_t>(writes.size()); ++binding) {
        writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set, .dstBinding = binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_infos[binding]};
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const auto code = load_shader();
    const VkShaderModuleCreateInfo shader_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(std::uint32_t), .pCode = code.data()};
    vcr::ShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device, &shader_info, nullptr,
        shader.put(device, vkDestroyShaderModule)));
    name(VK_OBJECT_TYPE_SHADER_MODULE, shader.get(), "exposure reference compute shader");
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &layout_handle};
    vcr::DeviceObject<VkPipelineLayout> pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
        pipeline_layout.put(device, vkDestroyPipelineLayout)));
    name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipeline_layout.get(), "exposure reference pipeline layout");
    const VkComputePipelineCreateInfo pipeline_info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader.get(), .pName = "main"},
        .layout = pipeline_layout.get()};
    vcr::DeviceObject<VkPipeline> pipeline;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        pipeline.put(device, vkDestroyPipeline)));
    name(VK_OBJECT_TYPE_PIPELINE, pipeline.get(), "exposure reference pipeline");

    const VkCommandPoolCreateInfo command_pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = context.queue_families().graphics};
    vcr::DeviceObject<VkCommandPool> command_pool;
    VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr,
        command_pool.put(device, vkDestroyCommandPool)));
    name(VK_OBJECT_TYPE_COMMAND_POOL, command_pool.get(), "exposure reference command pool");
    const VkCommandBufferAllocateInfo command_allocate{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool.get(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command{}; // Command pool owns this handle.
    VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate, &command));
    name(VK_OBJECT_TYPE_COMMAND_BUFFER, command, "exposure reference commands");
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout.get(),
                            0, 1, &descriptor_set, 0, nullptr);
    vkCmdDispatch(command, static_cast<std::uint32_t>(inputs.size()), 1, 1);
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(command, &dependency);
    VK_CHECK(vkEndCommandBuffer(command));

    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vcr::DeviceObject<VkFence> fence;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, fence.put(device, vkDestroyFence)));
    name(VK_OBJECT_TYPE_FENCE, fence.get(), "exposure reference completion");
    const VkCommandBufferSubmitInfo command_submit{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = command};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command_submit};
    SubmissionRetirement retirement{device};
    VK_CHECK(vkQueueSubmit2(context.graphics_queue(), 1, &submit, fence.get()));
    retirement.pending = true;
    const auto fence_handle = fence.get();
    VK_CHECK(vkWaitForFences(device, 1, &fence_handle, VK_TRUE, UINT64_MAX));
    retirement.pending = false;
    const auto bytes = output_buffer.read_completed_bytes(sizeof(actual));
    std::memcpy(actual.data(), bytes.data(), bytes.size());
    return verify_results(inputs, actual) ? 0 : 1;
}

} // namespace

int main() {
    try { return run(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
