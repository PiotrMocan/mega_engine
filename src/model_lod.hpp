#pragma once
#include "cooked_model_asset.hpp"
#include <span>
namespace vcr {
// Cold asset-preparation step, also usable by the background loader. Position,
// normal/UV seams and submesh borders are preserved; vertices are shared.
void prepare_model_lods(CookedModelAsset &model);
// Cold preparation. Names map LOD materials to LOD0; absent materials have a
// finite error and an empty range, distinguishing authored removal from no LOD.
// Each supplied model uses the same local coordinate system as LOD0.
void prepare_authored_model_lods(CookedModelAsset &model,
                                std::span<const CookedModelAsset> levels,
                                std::size_t maximum_geometry_bytes);
} // namespace vcr
