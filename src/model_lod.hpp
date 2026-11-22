#pragma once
#include "cooked_model_asset.hpp"
#include <span>
namespace vcr {
enum class ModelLodUvPolicy { standard, preserve_mapping };

// Cold asset-preparation step, also usable by the background loader. Position,
// normal/UV seams and submesh borders are preserved; vertices are shared.
void prepare_model_lods(CookedModelAsset &model, const std::filesystem::path& asset_root = {},
                        ModelLodUvPolicy uv_policy = ModelLodUvPolicy::standard);
void prepare_model_lods(CookedModelAsset &model, const std::filesystem::path& asset_root,
                        ModelLodUvPolicy uv_policy, bool local_foliage_coverage);
// Lossless cold-path indexing/cache preparation. Coarse LOD vertices are packed
// first so the frequently drawn distant instances fetch a compact working set.
void optimize_model_geometry(CookedModelAsset &model);
// Cold preparation of LOD1..LOD5. Legacy two-reference assets get a generated
// fourth level from LOD2. Names map materials to LOD0; absent materials have a
// finite error and an empty range, distinguishing authored removal from no LOD.
// Each supplied model uses the same local coordinate system as LOD0.
void prepare_authored_model_lods(CookedModelAsset &model,
                                std::span<const CookedModelAsset> levels,
                                std::size_t maximum_geometry_bytes,
                                const std::filesystem::path& asset_root = {});
} // namespace vcr
