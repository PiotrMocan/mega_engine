#pragma once
#include "cooked_model_asset.hpp"
namespace vcr {
// Cold asset-preparation step, also usable by the background loader. Position,
// normal/UV seams and submesh borders are preserved; vertices are shared.
void prepare_model_lods(CookedModelAsset &model);
} // namespace vcr
