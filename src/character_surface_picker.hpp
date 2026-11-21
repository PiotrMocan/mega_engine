#pragma once

#include "character_package.hpp"
#include "scene_visibility.hpp"

namespace vcr {

// Own the three source points so feedback remains attached across brush bakes
// even if regenerating the preview changes its vertex order.
struct CharacterSurfaceAnchor {
    glm::vec3 barycentric{};
    std::array<glm::vec3, 3> points{};
    std::array<CookedSkinVertex, 3> skin{};
    [[nodiscard]] glm::vec3 position(std::span<const glm::mat4> palette) const noexcept;
};

struct CharacterSurfaceHit {
    CharacterRaycastHit visible{};
    bool paintable_skin = false;
    glm::vec3 bind_position{}, bind_normal{};
    CharacterSurfaceAnchor anchor{};
};

// The spatial index and the mesh it indexes move together at preview publication.
class CharacterSurfacePicker final {
public:
    CharacterSurfacePicker() = default;
    CharacterSurfacePicker(CharacterSurfacePicker&&) noexcept = default;
    CharacterSurfacePicker& operator=(CharacterSurfacePicker&&) noexcept = default;
    CharacterSurfacePicker(const CharacterSurfacePicker&) = delete;
    CharacterSurfacePicker& operator=(const CharacterSurfacePicker&) = delete;
    [[nodiscard]] bool build(CharacterMeshBuildResult mesh, std::string& error);
    [[nodiscard]] const CharacterMeshBuildResult& mesh() const noexcept { return mesh_; }
    [[nodiscard]] std::optional<CharacterSurfaceHit> pick(glm::vec3 origin, glm::vec3 direction,
        std::span<const glm::mat4> palette) const noexcept;

private:
    struct Face {
        std::uint32_t first_index;
        CharacterPaintTarget target;
    };
    struct JointBox {
        Bounds bounds;
        std::uint32_t joint;
    };
    struct Range {
        std::size_t first, count, first_box, box_count;
    };
    CharacterMeshBuildResult mesh_;
    std::vector<Face> faces_;
    std::vector<Range> leaves_, groups_;
    std::vector<JointBox> boxes_;
};

} // namespace vcr
