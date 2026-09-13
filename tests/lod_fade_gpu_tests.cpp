#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include "lod_transition.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
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
    VkDevice device; // Observer; declared after every submitted resource.
    bool pending = false;
    ~SubmissionRetirement() {
        if (pending) {
            const auto result = vkDeviceWaitIdle(device);
            if (result != VK_SUCCESS)
                std::cerr << "LOD fade test retirement failed: VkResult " << result << '\n';
        }
    }
};

constexpr std::array<std::uint32_t, 8> sources{0, 1, 15, 16, 63, 64, 65537, 131071};
constexpr std::uint32_t phase_count = 4096, period_pixels = 64;
constexpr std::size_t coverage_count = sources.size() * 2;
constexpr auto no_failure = std::numeric_limits<std::uint32_t>::max();
// Explicit row-major ordered matrices are independent of lodKeep's bit loop.
constexpr std::array<std::uint32_t, 16> bayer4{
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};
constexpr std::array<std::uint32_t, 64> bayer8{
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
};
struct alignas(16) CoverageCase {
    std::array<std::uint32_t, 4> metadata{};
    std::array<std::uint32_t, period_pixels> first_phase{};
};
struct alignas(16) EncoderCase {
    std::array<std::uint32_t, 4> input{};
    std::array<std::uint32_t, 4> expected{};
};
struct alignas(16) Results {
    std::uint32_t coverage_pairs = 0, coverage_failures = 0, source_failures = 0;
    std::uint32_t complement_failures = 0, endpoint_failures = 0, off_checks = 0;
    std::uint32_t off_failures = 0, roundtrip_checks = 0, roundtrip_failures = 0;
    std::uint32_t encoder_checks = 0, encoder_failures = 0, mask_checks = 0, mask_failures = 0;
    std::uint32_t first_coverage = no_failure, first_source = no_failure;
    std::uint32_t first_complement = no_failure, first_endpoint = no_failure;
    std::uint32_t first_off = no_failure, first_roundtrip = no_failure;
    std::uint32_t first_encoder = no_failure, first_mask = no_failure;
    std::array<std::uint32_t, 3> padding{};
};
static_assert(sizeof(CoverageCase) == 272 && offsetof(CoverageCase, first_phase) == 16);
static_assert(sizeof(EncoderCase) == 32 && offsetof(EncoderCase, expected) == 16);
static_assert(sizeof(Results) == 96 && offsetof(Results, first_coverage) == 52);
static_assert(std::is_trivially_copyable_v<CoverageCase> &&
              std::is_trivially_copyable_v<EncoderCase> && std::is_trivially_copyable_v<Results>);

std::array<CoverageCase, coverage_count> make_coverage() {
    std::array<CoverageCase, coverage_count> inputs{};
    for (std::size_t source = 0; source < sources.size(); ++source) {
        for (std::uint32_t pattern = 1; pattern <= 2; ++pattern) {
            auto& input = inputs[source * 2 + pattern - 1];
            const std::uint32_t side = pattern == 1 ? 4 : 8;
            // Aligned origins preserve a complete period, while the high
            // origin makes shader seed addition exercise uint32 wraparound.
            const std::uint32_t origin = source % 3 == 0 ? 0 :
                (source % 3 == 1 ? 4096 : no_failure - 7);
            input.metadata = {sources[source], pattern, origin, origin};
            for (std::uint32_t pixel = 0; pixel < period_pixels; ++pixel) {
                const auto x = (std::uint64_t(origin) + pixel % 8 + sources[source] % side) % side;
                const auto y = (std::uint64_t(origin) + pixel / 8 + (sources[source] / side) % side) % side;
                const auto matrix_index = static_cast<std::size_t>(y * side + x);
                const auto rank = pattern == 1 ? bayer4[matrix_index] : bayer8[matrix_index];
                // The normalized threshold is (rank + 1/2) / area. Solve
                // q/4095 >= threshold using an exact integer ceiling.
                const auto numerator = (2 * rank + 1) * 4095U;
                const auto denominator = 2 * side * side;
                input.first_phase[pixel] = (numerator + denominator - 1) / denominator;
            }
        }
    }
    return inputs;
}

std::uint32_t expected_token(std::uint32_t source, float phase, bool previous,
                             vcr::LodFadePattern pattern) {
    // Independent arithmetic packing. The finite float input is exact in
    // double; this reference does not call either production encoder.
    const double clamped = std::clamp(static_cast<double>(phase), 0.0, 1.0);
    const auto quantized = static_cast<std::uint32_t>(std::floor(clamped * 4095.0 + 0.5));
    return source + quantized * 131072U + (previous ? 0x20000000U : 0U) +
        (pattern == vcr::LodFadePattern::bayer8 ? 0x40000000U : 0U) +
        (pattern != vcr::LodFadePattern::off ? 0x80000000U : 0U);
}

constexpr std::size_t finite_phase_count = 15;
constexpr std::size_t encoder_count = sources.size() * 4 * finite_phase_count + 4095 * 3 * 4;
static_assert(encoder_count <= coverage_count * phase_count);
static_assert(coverage_count * phase_count * period_pixels <= no_failure);

std::vector<EncoderCase> make_encoders() {
    std::vector<EncoderCase> inputs;
    inputs.reserve(encoder_count);
    constexpr std::array<std::uint32_t, 8> raw_tokens{
        0, 0x1ffffU, 0x20000U, 0x80000000U, 0xffffffffU, 0x40012345U, 0x20010001U, 0x7fffffffU};
    const auto append = [&](std::uint32_t source, float phase) {
        for (const auto pattern : {vcr::LodFadePattern::bayer4, vcr::LodFadePattern::bayer8}) {
            for (const bool previous : {false, true}) {
                const auto raw = raw_tokens[inputs.size() % raw_tokens.size()];
                inputs.push_back({{source, std::bit_cast<std::uint32_t>(phase),
                                   static_cast<std::uint32_t>(pattern), previous ? 1U : 0U},
                                  {expected_token(source, phase, previous, pattern), raw,
                                   raw % 131072U, 0U}});
            }
        }
    };
    const std::array<float, finite_phase_count> phases{
        -std::numeric_limits<float>::max(), -1.0F, -std::numeric_limits<float>::epsilon(),
        0.0F, std::nextafter(0.0F, 1.0F), std::numeric_limits<float>::min(),
        .125F, .25F, .5F, .75F, std::nextafter(1.0F, 0.0F), 1.0F,
        std::nextafter(1.0F, 2.0F), 2.0F, std::numeric_limits<float>::max()};
    for (const auto source : sources)
        for (const auto phase : phases) append(source, phase);
    // Every half-step and its adjacent floats are strict CPU/GPU parity
    // cases. Sources rotate; both role/pattern flag combinations are tested.
    for (std::uint32_t q = 0; q < 4095; ++q) {
        const float tie = static_cast<float>((static_cast<double>(q) + .5) / 4095.0);
        for (const auto phase : {std::nextafter(tie, 0.0F), tie, std::nextafter(tie, 1.0F)})
            append(sources[q % sources.size()], phase);
    }
    if (inputs.size() != encoder_count) vcr::fatal("LOD encoder fixture size mismatch");
    return inputs;
}

void check_cpu_encoder(std::span<const EncoderCase> inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& value = inputs[i];
        const auto token = vcr::encode_lod_visibility(value.input[0],
            std::bit_cast<float>(value.input[1]), value.input[3] != 0,
            static_cast<vcr::LodFadePattern>(value.input[2]));
        if (!token || *token != value.expected[0] ||
            vcr::lod_visibility_source(value.expected[1]) != value.expected[2]) {
            std::cerr << "CPU LOD encoder/source mismatch case=" << i << '\n';
            vcr::fatal("CPU LOD encoder reference failed");
        }
    }
    for (const auto pattern : {vcr::LodFadePattern::off, vcr::LodFadePattern::bayer4,
                               vcr::LodFadePattern::bayer8}) {
        for (const auto source : {131072U, no_failure})
            if (vcr::encode_lod_visibility(source, .5F, false, pattern))
                vcr::fatal("CPU LOD encoder accepted an oversized source ID");
    }
    if (vcr::encode_lod_visibility(0, .5F, false, static_cast<vcr::LodFadePattern>(3)))
        vcr::fatal("CPU LOD encoder accepted an unknown pattern");
    for (const auto source : sources) {
        const auto token = vcr::encode_lod_visibility(source, 0, false, vcr::LodFadePattern::off);
        if (!token || *token != source) vcr::fatal("CPU plain off token changed its source ID");
    }
}

bool verify_results(const Results& result, std::span<const CoverageCase> coverage,
                     std::span<const EncoderCase> encoders) {
    const std::array failures{
        std::pair{"coverage", std::pair{result.coverage_failures, result.first_coverage}},
        std::pair{"source", std::pair{result.source_failures, result.first_source}},
        std::pair{"complement", std::pair{result.complement_failures, result.first_complement}},
        std::pair{"endpoint", std::pair{result.endpoint_failures, result.first_endpoint}},
        std::pair{"off", std::pair{result.off_failures, result.first_off}},
        std::pair{"roundtrip", std::pair{result.roundtrip_failures, result.first_roundtrip}},
        std::pair{"encoder", std::pair{result.encoder_failures, result.first_encoder}},
        std::pair{"mask", std::pair{result.mask_failures, result.first_mask}},
    };
    bool passed = result.coverage_pairs == coverage_count * phase_count * period_pixels &&
        result.off_checks == coverage_count * period_pixels &&
        result.roundtrip_checks == coverage_count * phase_count * 2 &&
        result.encoder_checks == encoders.size() && result.mask_checks == encoders.size();
    if (!passed) std::cerr << "LOD GPU test did not execute every required case\n";
    for (const auto& [label, detail] : failures) {
        const auto [count, first] = detail;
        if (count != 0 || first != no_failure) {
            passed = false;
            std::cerr << "LOD GPU " << label << " failures=" << count << " first=" << first << '\n';
        }
    }
    if (result.first_coverage != no_failure) {
        const auto pixel = result.first_coverage % period_pixels;
        const auto invocation = result.first_coverage / period_pixels;
        const auto case_index = invocation / phase_count;
        if (case_index < coverage.size()) {
            const auto& input = coverage[case_index];
            std::cerr << "Coverage source=" << input.metadata[0] << " pattern=" << input.metadata[1]
                      << " phase=" << invocation % phase_count << " pixel=" << pixel
                      << " first keep phase=" << input.first_phase[pixel] << '\n';
        }
    }
    if (result.first_encoder < encoders.size()) {
        const auto& input = encoders[result.first_encoder];
        std::cerr << "Encoder source=" << input.input[0] << " phase="
                  << std::hexfloat << std::bit_cast<float>(input.input[1]) << std::defaultfloat
                  << " pattern=" << input.input[2] << " previous=" << input.input[3]
                  << " expected token=" << input.expected[0] << '\n';
    }
    return passed;
}

int run() {
    const auto coverage = make_coverage();
    const auto encoders = make_encoders();
    check_cpu_encoder(encoders);
    SdlLifetime sdl;
    std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
        "LOD fade numerical reference", 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
    if (!window) vcr::fatal(SDL_GetError());
    vcr::VulkanContext context;
    context.initialize(window.get(), true);
    if (!context.validation_enabled()) vcr::fatal("LOD fade GPU test requires validation layer");
    const auto device = context.device();
    const auto& limits = context.properties().limits;
    if (limits.maxComputeWorkGroupInvocations < 64 || limits.maxComputeWorkGroupSize[0] < 64 ||
        limits.maxComputeWorkGroupCount[0] < phase_count / 64 ||
        limits.maxComputeWorkGroupCount[1] < coverage_count ||
        limits.maxStorageBufferRange < encoder_count * sizeof(EncoderCase))
        vcr::fatal("LOD fade GPU test exceeds device compute/storage limits");
    const auto name = [&](VkObjectType type, auto handle, const char* label) {
        context.set_debug_name(type, vcr::handle_value(handle), label);
    };
    vcr::GpuBuffer coverage_buffer, encoder_buffer, output_buffer;
    const auto upload = [&](vcr::GpuBuffer& buffer, auto bytes, const char* label) {
        buffer.create(context, bytes.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT, label);
        buffer.write(bytes);
    };
    upload(coverage_buffer, std::as_bytes(std::span{coverage}), "LOD coverage thresholds");
    upload(encoder_buffer, std::as_bytes(std::span{encoders}), "LOD encoder reference cases");
    output_buffer.create(context, sizeof(Results), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT, "LOD reference counters");
    const Results initial{};
    output_buffer.write(std::as_bytes(std::span{&initial, 1}));
    constexpr std::uint32_t binding_count = 3;
    std::array<VkDescriptorSetLayoutBinding, binding_count> bindings{};
    for (std::uint32_t i = 0; i < binding_count; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count, .pBindings = bindings.data()};
    vcr::DeviceObject<VkDescriptorSetLayout> layout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
        layout.put(device, vkDestroyDescriptorSetLayout)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, layout.get(), "LOD reference descriptor layout");
    const auto layout_handle = layout.get();
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding_count};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    vcr::DeviceObject<VkDescriptorPool> pool;
    VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, pool.put(device, vkDestroyDescriptorPool)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_POOL, pool.get(), "LOD reference descriptor pool");
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool.get(), .descriptorSetCount = 1, .pSetLayouts = &layout_handle};
    VkDescriptorSet set{}; // Descriptor pool owns this handle.
    VK_CHECK(vkAllocateDescriptorSets(device, &allocate, &set));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET, set, "LOD reference descriptors");
    const std::array buffer_infos{
        VkDescriptorBufferInfo{coverage_buffer.handle(), 0, coverage_buffer.size()},
        VkDescriptorBufferInfo{encoder_buffer.handle(), 0, encoder_buffer.size()},
        VkDescriptorBufferInfo{output_buffer.handle(), 0, output_buffer.size()}};
    std::array<VkWriteDescriptorSet, binding_count> writes{};
    for (std::uint32_t i = 0; i < binding_count; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_infos[i]};
    vkUpdateDescriptorSets(device, binding_count, writes.data(), 0, nullptr);

    std::ifstream file(VCR_LOD_FADE_TEST_SPV, std::ios::binary | std::ios::ate);
    const std::streamoff length = file.tellg();
    constexpr std::streamoff maximum_shader_bytes = 4 * 1024 * 1024;
    if (!file || length <= 0 || length > maximum_shader_bytes || length % 4 != 0)
        vcr::fatal("Missing or oversized LOD reference SPIR-V; compile shaders first");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(length)))
        vcr::fatal("Cannot read LOD reference SPIR-V");
    const VkShaderModuleCreateInfo shader_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(std::uint32_t), .pCode = code.data()};
    vcr::ShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device, &shader_info, nullptr, shader.put(device, vkDestroyShaderModule)));
    name(VK_OBJECT_TYPE_SHADER_MODULE, shader.get(), "LOD reference compute shader");
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout_handle};
    vcr::DeviceObject<VkPipelineLayout> pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
        pipeline_layout.put(device, vkDestroyPipelineLayout)));
    name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipeline_layout.get(), "LOD reference pipeline layout");
    const VkComputePipelineCreateInfo pipeline_info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader.get(), .pName = "main"}, .layout = pipeline_layout.get()};
    vcr::DeviceObject<VkPipeline> pipeline;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        pipeline.put(device, vkDestroyPipeline)));
    name(VK_OBJECT_TYPE_PIPELINE, pipeline.get(), "LOD reference pipeline");
    const VkCommandPoolCreateInfo command_pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = context.queue_families().graphics};
    vcr::DeviceObject<VkCommandPool> command_pool;
    VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr,
        command_pool.put(device, vkDestroyCommandPool)));
    name(VK_OBJECT_TYPE_COMMAND_POOL, command_pool.get(), "LOD reference command pool");
    const VkCommandBufferAllocateInfo command_allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool.get(), .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command{}; // Command pool owns this handle.
    VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate, &command));
    name(VK_OBJECT_TYPE_COMMAND_BUFFER, command, "LOD reference commands");
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command, &begin));
    // GpuBuffer::write flushes host writes, including the atomic counters.
    // Queue submission makes these writes available to this dispatch.
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout.get(),
        0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, phase_count / 64, static_cast<std::uint32_t>(coverage_count), 1);
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(command, &dependency);
    VK_CHECK(vkEndCommandBuffer(command));
    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vcr::DeviceObject<VkFence> fence;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, fence.put(device, vkDestroyFence)));
    name(VK_OBJECT_TYPE_FENCE, fence.get(), "LOD reference completion");
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
    Results result{};
    const auto bytes = output_buffer.read_completed_bytes(sizeof(result));
    std::memcpy(&result, bytes.data(), bytes.size());
    if (!verify_results(result, coverage, encoders)) return 1;
    std::cout << "LOD fade GPU reference: " << result.coverage_pairs
              << " complementary pixel pairs, " << result.roundtrip_checks
              << " quantized encoder roundtrips, " << result.encoder_checks
              << " strict encoder cases, " << result.mask_checks << " source-mask cases, "
              << result.off_checks << " plain off pixels passed; 96-byte readback\n";
    return 0;
}
} // namespace

int main() {
    try { return run(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
