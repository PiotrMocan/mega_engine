#include "device_object.hpp"
#include "gpu_buffer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
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
    VkDevice device; // Observer; context and all submitted resources outlive this guard.
    bool pending = false;
    ~SubmissionRetirement() {
        if (pending) {
            // Exceptional test teardown must retire work before the RAII buffers
            // and command pool unwind, even if the fence wait itself failed.
            const auto result = vkDeviceWaitIdle(device);
            if (result != VK_SUCCESS)
                std::cerr << "Test retirement failed: VkResult " << result << '\n';
        }
    }
};
using Pixel = std::array<float, 4>;
struct Reference { Pixel input, expected; };

// Independent double-precision evaluation of Polyphony's reference, documented
// in docs/GT7_TONE_MAPPING.md; these are not recomputed by a copy of our shader.
constexpr std::array references{
    Reference{{0,0,0,1},{0,0,0,1}},
    Reference{{.01F,.01F,.01F,1},{.001313273F,.001313263F,.001313262F,1}},
    Reference{{.18F,.18F,.18F,1},{.057949922F,.057949519F,.057949441F,1}},
    Reference{{.538F,.538F,.538F,1},{.215201268F,.215199771F,.215199480F,1}},
    Reference{{1,1,1,1},{.400002356F,.399999574F,.399999034F,1}},
    Reference{{2.5F,2.5F,2.5F,1},{.835157181F,.835151373F,.835150245F,1}},
    Reference{{10,10,10,1},{1,1,1,1}},
    Reference{{1,0,0,1},{.381941434F,0,0,1}},
    Reference{{0,1,0,1},{0,.397866654F,0,1}},
    Reference{{0,0,1,1},{0,0,.377845307F,1}},
    Reference{{10,0,0,1},{1,.100817694F,.004803368F,1}},
    Reference{{0,10,0,1},{1,1,.736703186F,1}},
    Reference{{0,0,10,1},{.106083527F,0,1,1}},
    Reference{{10,1,.1F,1},{1,.465588481F,.072115459F,1}},
    Reference{{65504,65504,65504,1},{1,1,1,1}},
    Reference{{-1,.18F,2.5F,1},{0,.058932707F,.865982263F,1}},
    Reference{{1e30F,0,0,1},{1,1,1,1}},
    Reference{{std::numeric_limits<float>::infinity(),0,0,1},{1,1,1,1}},
    Reference{{std::numeric_limits<float>::quiet_NaN(),0,0,1},{0,0,0,1}},
};

int run() {
    SdlLifetime sdl;
    std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
        "Tone mapping reference", 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
    if (!window) vcr::fatal(SDL_GetError());
    vcr::VulkanContext context;
    context.initialize(window.get(), true);
    if (!context.validation_enabled()) vcr::fatal("GPU reference test requires validation layer");
    const auto device = context.device();
    const auto name = [&](VkObjectType type, auto handle, const char* label) {
        context.set_debug_name(type, vcr::handle_value(handle), label);
    };

    std::array<Pixel, references.size()> input{};
    for (std::size_t i=0; i<input.size(); ++i) input[i]=references[i].input;
    vcr::GpuBuffer inputs, outputs;
    inputs.create(context, sizeof(input), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT, "tone reference inputs");
    outputs.create(context, sizeof(input), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT, "tone reference outputs");
    inputs.write(std::as_bytes(std::span(input)));
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings.data()};
    vcr::DeviceObject<VkDescriptorSetLayout> layout;
    VK_CHECK(vkCreateDescriptorSetLayout(device,&layout_info,nullptr,
        layout.put(device,vkDestroyDescriptorSetLayout)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,layout.get(),"tone reference descriptor layout");
    const auto layout_handle=layout.get();
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    const VkDescriptorPoolCreateInfo pool_info{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    vcr::DeviceObject<VkDescriptorPool> pool;
    VK_CHECK(vkCreateDescriptorPool(device,&pool_info,nullptr,pool.put(device,vkDestroyDescriptorPool)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_POOL,pool.get(),"tone reference descriptor pool");
    const VkDescriptorSetAllocateInfo allocate{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool.get(),.descriptorSetCount=1,.pSetLayouts=&layout_handle};
    VkDescriptorSet set{}; // Owned by descriptor pool, never individually freed.
    VK_CHECK(vkAllocateDescriptorSets(device,&allocate,&set));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET,set,"tone reference descriptors");
    const std::array buffer_infos{VkDescriptorBufferInfo{inputs.handle(),0,inputs.size()},
                                  VkDescriptorBufferInfo{outputs.handle(),0,outputs.size()}};
    std::array<VkWriteDescriptorSet,2> writes{};
    for (std::uint32_t i=0; i<2; ++i) {
        writes[i]={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo=&buffer_infos[i]};
    }
    vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);

    std::ifstream file(VCR_TONE_MAPPING_TEST_SPV,std::ios::binary|std::ios::ate);
    const auto length=file.tellg();
    if (!file || length<=0 || length%4!=0) vcr::fatal("Missing reference SPIR-V; compile shaders first");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(length)/4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()),length)) vcr::fatal("Cannot read reference SPIR-V");
    const VkShaderModuleCreateInfo shader_info{.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=code.size()*sizeof(std::uint32_t),.pCode=code.data()};
    vcr::ShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device,&shader_info,nullptr,shader.put(device,vkDestroyShaderModule)));
    name(VK_OBJECT_TYPE_SHADER_MODULE,shader.get(),"tone reference compute shader");
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout_handle};
    vcr::DeviceObject<VkPipelineLayout> pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device,&pipeline_layout_info,nullptr,
        pipeline_layout.put(device,vkDestroyPipelineLayout)));
    name(VK_OBJECT_TYPE_PIPELINE_LAYOUT,pipeline_layout.get(),"tone reference pipeline layout");
    const VkComputePipelineCreateInfo pipeline_info{.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
            .module=shader.get(),.pName="main"},.layout=pipeline_layout.get()};
    vcr::DeviceObject<VkPipeline> pipeline;
    VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,
        pipeline.put(device,vkDestroyPipeline)));
    name(VK_OBJECT_TYPE_PIPELINE,pipeline.get(),"tone reference pipeline");

    const VkCommandPoolCreateInfo command_pool_info{.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=context.queue_families().graphics};
    vcr::DeviceObject<VkCommandPool> command_pool;
    VK_CHECK(vkCreateCommandPool(device,&command_pool_info,nullptr,
        command_pool.put(device,vkDestroyCommandPool)));
    name(VK_OBJECT_TYPE_COMMAND_POOL,command_pool.get(),"tone reference command pool");
    const VkCommandBufferAllocateInfo command_allocate{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=command_pool.get(),.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command{}; // Owned by command pool.
    VK_CHECK(vkAllocateCommandBuffers(device,&command_allocate,&command));
    name(VK_OBJECT_TYPE_COMMAND_BUFFER,command,"tone reference commands");
    const VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command,&begin));
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.get());
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout.get(),0,1,&set,0,nullptr);
    vkCmdDispatch(command,static_cast<std::uint32_t>(input.size()),1,1);
    const VkMemoryBarrier2 barrier{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount=1,.pMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(command,&dependency);
    VK_CHECK(vkEndCommandBuffer(command));
    const VkFenceCreateInfo fence_info{.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vcr::DeviceObject<VkFence> fence;
    VK_CHECK(vkCreateFence(device,&fence_info,nullptr,fence.put(device,vkDestroyFence)));
    name(VK_OBJECT_TYPE_FENCE,fence.get(),"tone reference completion");
    const VkCommandBufferSubmitInfo command_submit{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer=command};
    const VkSubmitInfo2 submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount=1,.pCommandBufferInfos=&command_submit};
    SubmissionRetirement retirement{device};
    VK_CHECK(vkQueueSubmit2(context.graphics_queue(),1,&submit,fence.get()));
    retirement.pending=true;
    const auto fence_handle=fence.get();
    VK_CHECK(vkWaitForFences(device,1,&fence_handle,VK_TRUE,UINT64_MAX));
    retirement.pending=false;
    const auto bytes=outputs.read_completed_bytes(sizeof(input));
    std::array<Pixel,references.size()> actual{};
    std::memcpy(actual.data(),bytes.data(),bytes.size());
    float maximum_error=0;
    for (std::size_t i=0;i<actual.size();++i) {
        for (std::size_t channel=0;channel<4;++channel) {
            const auto value=actual[i][channel];
            const auto error=std::abs(value-references[i].expected[channel]);
            if (!std::isfinite(value) || value<0 || value>1 || error>5e-4F) {
                std::cerr<<"GT7 reference mismatch sample="<<i<<" channel="<<channel
                         <<" expected="<<references[i].expected[channel]<<" actual="<<value<<'\n';
                return 1;
            }
            maximum_error=std::max(maximum_error,error);
        }
    }
    std::cout<<std::scientific<<std::setprecision(9)
             <<"GT7 GPU reference: "<<actual.size()<<" samples passed; max absolute error="
             <<maximum_error<<'\n';
    return 0;
}
} // namespace

int main() {
    try { return run(); }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
