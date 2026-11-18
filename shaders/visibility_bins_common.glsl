#ifndef VCR_VISIBILITY_BINS_COMMON_GLSL
#define VCR_VISIBILITY_BINS_COMMON_GLSL

// Must match source_capacity (max_scene_nodes + 1 = 100001) and
// max_binned_commands (max_shadow_draw_groups * kModelLods) in renderer_gpu_visibility.cpp.
const uint kMaxSceneSources = 100001u;
const uint kBinCountsBase = 5u * kModelLods * kMaxSceneSources;
const uint kDepthBins = 16u;
const uint kMaxBinnedCommands = 4096u * kModelLods;
const uint kScratchBase = kBinCountsBase + kMaxBinnedCommands * kDepthBins;

#endif
