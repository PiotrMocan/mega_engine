#pragma once

#include "gameplay_session.hpp"
#include "renderer.hpp"

#include <algorithm>

namespace vcr {

[[nodiscard]] inline SkeletalAssetProvider make_renderer_skeletal_provider(
    Renderer* const renderer) noexcept {
    if (renderer == nullptr) {
        return {};
    }
    return SkeletalAssetProvider{
        .asset_context = renderer,
        .pose_context = renderer,
        .find_skeleton = +[](const void* const ctx,
                             const std::string_view path) noexcept
                             -> const CookedSkeletonAsset* {
            return static_cast<const Renderer*>(ctx)->find_model_skeleton(path);
        },
        .find_animations = +[](const void* const ctx,
                               const std::string_view path) noexcept
                               -> const std::vector<AnimationClip>* {
            return static_cast<const Renderer*>(ctx)
                ->find_model_animations(path);
        },
        .submit_pose = +[](void* const ctx,
                           const SceneNodeId id,
                           const std::string_view path,
                           const std::span<const glm::mat4> skin) {
            static_cast<Renderer*>(ctx)->set_skinned_model_pose(id, path, skin);
        },
        .remove_pose = +[](void* const ctx,
                           const SceneNodeId id) noexcept {
            static_cast<Renderer*>(ctx)->remove_skinned_model_pose(id);
        },
    };
}

[[nodiscard]] inline GameplayRendererBridge make_gameplay_renderer_bridge(
    Renderer& renderer) noexcept {
    return GameplayRendererBridge{
        .context = &renderer,
        .bounds_provider = ModelBoundsProvider{
            .context = &renderer,
            .query = +[](const void* const ctx,
                         const SceneNodeId id) noexcept
                         -> std::optional<Bounds> {
                return static_cast<const Renderer*>(ctx)
                    ->editor_node_bounds(id);
            },
        },
        .collision_provider = ModelCollisionProvider{
            .context = &renderer,
            .query = +[](const void* const ctx,
                         const std::string_view path) noexcept
                         -> const CookedModelCollision* {
                return static_cast<const Renderer*>(ctx)
                    ->editor_model_collision(path);
            },
        },
        .transform_sink = RuntimeTransformSink{
            .context = &renderer,
            .apply = +[](void* const ctx,
                         const SceneNodeId id,
                         const TransformRows rows) noexcept -> bool {
                return static_cast<Renderer*>(ctx)
                    ->set_runtime_transform(id, rows);
            },
        },
        .skeletal_provider = make_renderer_skeletal_provider(&renderer),
        .query_output_extent = +[](const void* const ctx) noexcept
                                   -> glm::uvec2 {
            const VkExtent2D ext =
                static_cast<const Renderer*>(ctx)->output_extent();
            return glm::uvec2{
                std::max(1U, ext.width),
                std::max(1U, ext.height),
            };
        },
        .submit_overlay_vertices = +[](void* const ctx,
                                       const std::span<const GameUiVertex> verts) {
            static_cast<Renderer*>(ctx)->set_game_overlay_vertices(verts);
        },
        .clear_skinned_poses = +[](void* const ctx) noexcept {
            static_cast<Renderer*>(ctx)->clear_skinned_model_poses();
        },
    };
}

} // namespace vcr
