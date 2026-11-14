#pragma once

#include "collision.hpp"
#include "gameplay.hpp"
#include "runtime_scene.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vcr {

enum class PhysicsShapeKind : std::uint8_t {
    box = 0U,
    sphere = 1U,
    capsule = 2U,
};

struct PhysicsBodyState {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    PhysicsShapeKind shape = PhysicsShapeKind::box;
    RigidBodyMotionType motion_type = RigidBodyMotionType::dynamic;
    glm::vec3 position{0.0F};
    glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 linear_velocity{0.0F};
    glm::vec3 angular_velocity{0.0F};
    glm::vec3 half_extents{0.5F};
    float radius = 0.5F;
    float half_segment = 0.5F;
    bool sleeping = false;
};

class PhysicsWorld final {
public:
    PhysicsWorld();

    void clear() noexcept;

    void rebuild_from_scene(
        const SceneDocument& scene,
        SceneNodeId excluded_node = {},
        ModelBoundsProvider model_bounds = {},
        ModelCollisionProvider model_collision = {});

    void step(float delta_seconds, const CollisionWorld& static_world) noexcept;

    void apply_character_push(
        const CharacterController& character,
        glm::vec3 desired_planar_velocity,
        float delta_seconds,
        float character_mass = 75.0F,
        std::uint32_t layer_mask = 0xFFFFFFFFU) noexcept;

    void resolve_character_post_step(
        CharacterController& character,
        const CollisionWorld& static_world,
        float delta_seconds,
        std::uint32_t layer_mask = 0xFFFFFFFFU) const noexcept;

    [[nodiscard]] std::optional<RaycastHit> raycast(
        glm::vec3 origin,
        glm::vec3 direction,
        float max_distance,
        std::uint32_t layer_mask = 0xFFFFFFFFU) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return bodies_.empty(); }

    [[nodiscard]] std::size_t body_count() const noexcept {
        return bodies_.size();
    }

    [[nodiscard]] SceneNodeId body_node_id(std::size_t index) const noexcept;

    [[nodiscard]] TransformRows body_transform_rows(
        std::size_t index) const noexcept;

    [[nodiscard]] std::optional<PhysicsBodyState> body_state(
        SceneNodeId id) const noexcept;

    bool set_linear_velocity(SceneNodeId id, glm::vec3 velocity) noexcept;
    bool set_angular_velocity(
        SceneNodeId id,
        glm::vec3 angular_velocity) noexcept;
    bool set_kinematic_pose(
        SceneNodeId id,
        glm::vec3 position,
        glm::quat orientation,
        float delta_seconds) noexcept;
    bool set_body_state(
        SceneNodeId id,
        glm::vec3 position,
        glm::quat orientation,
        glm::vec3 linear_velocity,
        glm::vec3 angular_velocity) noexcept;

private:
    struct Body {
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        PhysicsShapeKind shape = PhysicsShapeKind::box;
        RigidBodyMotionType motion_type = RigidBodyMotionType::dynamic;
        glm::vec3 position{0.0F};
        glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
        glm::mat3 axes{1.0F};
        glm::mat3 world_inv_inertia{0.0F};
        glm::vec3 local_inv_inertia{0.0F};
        glm::vec3 node_scale{1.0F};
        glm::vec3 rot_frame_offset{0.0F};
        glm::vec3 linear_velocity{0.0F};
        glm::vec3 angular_velocity{0.0F};
        glm::vec3 half_extents{0.5F};
        float radius = 0.5F;
        float half_segment = 0.5F;
        float inv_mass = 1.0F;
        float linear_damping = 0.05F;
        float angular_damping = 0.10F;
        float restitution = 0.15F;
        float friction = 0.60F;
        float gravity_scale = 1.0F;
        float sleep_timer = 0.0F;
        Bounds world_aabb{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool lock_rotation = false;
        bool sleeping = false;
        bool had_contact = false;
    };

    struct ContactConstraint {
        std::uint32_t body_a = 0U;
        std::uint32_t body_b = UINT32_MAX;
        glm::vec3 point{0.0F};
        glm::vec3 normal{0.0F, 1.0F, 0.0F};
        glm::vec3 tangent_1{1.0F, 0.0F, 0.0F};
        glm::vec3 tangent_2{0.0F, 0.0F, 1.0F};
        glm::vec3 r_a{0.0F};
        glm::vec3 r_b{0.0F};
        float penetration = 0.0F;
        float normal_mass = 0.0F;
        float tangent_mass_1 = 0.0F;
        float tangent_mass_2 = 0.0F;
        float velocity_bias = 0.0F;
        float friction = 0.6F;
        float accum_normal = 0.0F;
        float accum_tangent_1 = 0.0F;
        float accum_tangent_2 = 0.0F;
    };

    void substep(float dt, const CollisionWorld& static_world) noexcept;
    void update_derived_body_state(Body& body) noexcept;
    void gather_static_contacts(
        std::uint32_t body_idx,
        const CollisionWorld& static_world) noexcept;
    void gather_pair_contacts(
        std::uint32_t idx_a,
        std::uint32_t idx_b) noexcept;
    void push_contact(
        std::uint32_t idx_a,
        std::uint32_t idx_b,
        glm::vec3 point,
        glm::vec3 normal_b_to_a,
        float penetration,
        float dt) noexcept;

    std::vector<Body> bodies_{};
    std::vector<ContactConstraint> contacts_{};
    std::vector<SphereContact> sphere_contacts_scratch_{};
    std::vector<SceneWorldNode> world_nodes_scratch_{};
    std::vector<std::uint32_t> ancestor_scratch_{};
    float accumulator_ = 0.0F;
};

} // namespace vcr
