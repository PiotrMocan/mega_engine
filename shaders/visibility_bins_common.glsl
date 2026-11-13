#ifndef VCR_VISIBILITY_BINS_COMMON_GLSL
#define VCR_VISIBILITY_BINS_COMMON_GLSL

// Must match source_capacity (max_scene_nodes + 1 = 100001) and
// max_binned_commands (max_shadow_draw_groups * 4 = 16384) in renderer_gpu_visibility.cpp.
const uint kMaxSceneSources = 100001u;
const uint kBinCountsBase = 20u * kMaxSceneSources;
const uint kDepthBins = 16u;
const uint kMaxBinnedCommands = 4096u * 4u;
const uint kScratchBase = kBinCountsBase + kMaxBinnedCommands * kDepthBins;

#endif
