#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::string_view project_manifest_filename =
    "project.vcrproject.json";
inline constexpr std::string_view project_manifest_format =
    "vulkan_cube_project";
inline constexpr std::uint32_t project_manifest_version = 1U;

inline constexpr std::string_view project_registry_filename =
    "project_registry.json";
inline constexpr std::string_view project_registry_format =
    "vulkan_cube_project_registry";
inline constexpr std::uint32_t project_registry_version = 1U;

inline constexpr std::size_t maximum_registered_projects = 1024U;
inline constexpr std::size_t maximum_project_name_bytes = 128U;
inline constexpr std::size_t maximum_project_description_bytes = 1024U;
inline constexpr std::uintmax_t maximum_project_manifest_bytes =
    1U * 1024U * 1024U;
inline constexpr std::uintmax_t maximum_project_registry_bytes =
    8U * 1024U * 1024U;

enum class ProjectTemplateKind : std::uint8_t {
    standard,
    empty,
};

[[nodiscard]] std::string_view to_token(ProjectTemplateKind kind) noexcept;
[[nodiscard]] std::optional<ProjectTemplateKind> parse_project_template_kind(
    std::string_view token) noexcept;
[[nodiscard]] const char* project_template_display_name(
    ProjectTemplateKind kind) noexcept;
[[nodiscard]] const char* project_template_description(
    ProjectTemplateKind kind) noexcept;

struct ProjectManifest {
    std::string name{};
    std::string description{};
    std::string template_id{"standard"};
    std::string startup_scene{"scenes/editor_scene.json"};
    std::string created_utc{};
};

enum class ProjectDiskState : std::uint8_t {
    ready,
    missing_directory,
    missing_manifest,
    invalid_manifest,
    missing_assets,
};

[[nodiscard]] const char* project_disk_state_label(
    ProjectDiskState state) noexcept;

struct ProjectInspection {
    ProjectDiskState state = ProjectDiskState::missing_directory;
    ProjectManifest manifest{};
    std::filesystem::path project_directory{};
    std::filesystem::path assets_directory{};
    std::vector<std::string> scene_relative_paths{};
    bool startup_scene_exists = false;
    std::string diagnostic{};
};

struct ProjectRegistryEntry {
    std::string name{};
    std::string description{};
    std::filesystem::path project_directory{};
    std::string template_id{"standard"};
    std::string startup_scene{"scenes/editor_scene.json"};
    std::string last_opened_utc{};
    bool pinned = false;
};

struct CreateProjectOptions {
    std::string name{};
    std::string description{};
    std::filesystem::path parent_directory{};
    std::string folder_name{};
    ProjectTemplateKind template_kind = ProjectTemplateKind::standard;
};

struct CreateProjectResult {
    std::filesystem::path project_directory{};
    ProjectManifest manifest{};
};

class ProjectRegistry final {
public:
    [[nodiscard]] bool load_or_initialize(
        const std::filesystem::path& registry_file,
        std::string& error);
    [[nodiscard]] bool save(
        const std::filesystem::path& registry_file,
        std::string& error) const;

    [[nodiscard]] bool register_project_directory(
        const std::filesystem::path& project_directory,
        std::string& error,
        bool mark_opened = false);

    [[nodiscard]] bool upsert_entry(
        ProjectRegistryEntry entry,
        std::string& error);

    [[nodiscard]] bool remove_by_directory(
        const std::filesystem::path& project_directory) noexcept;

    [[nodiscard]] bool set_pinned(
        const std::filesystem::path& project_directory,
        bool pinned) noexcept;

    [[nodiscard]] bool touch_last_opened(
        const std::filesystem::path& project_directory);

    // Scans immediate subdirectories of parent_directory for valid project
    // manifests and registers any discovered projects.
    [[nodiscard]] std::size_t scan_directory(
        const std::filesystem::path& parent_directory,
        std::string& error);

    [[nodiscard]] std::span<const ProjectRegistryEntry> entries() const noexcept {
        return entries_;
    }

    void clear() noexcept {
        entries_.clear();
    }

private:
    std::vector<ProjectRegistryEntry> entries_{};
};

// Converts an arbitrary UTF-8 project name into a clean filesystem slug.
[[nodiscard]] std::string make_project_folder_slug(std::string_view name);

// Validates a relative scene path under <project>/assets (e.g. "scenes/foo.json").
[[nodiscard]] bool valid_project_scene_relative_path(
    std::string_view path) noexcept;

[[nodiscard]] bool save_project_manifest(
    const ProjectManifest& manifest,
    const std::filesystem::path& manifest_file,
    std::string& error);

[[nodiscard]] bool load_project_manifest(
    const std::filesystem::path& manifest_file,
    ProjectManifest& output,
    std::string& error);

[[nodiscard]] ProjectInspection inspect_project_directory(
    const std::filesystem::path& candidate_path);

// Creates a new self-contained project directory with manifest, assets, and
// starter scene/assets according to options.template_kind.
[[nodiscard]] bool create_project_on_disk(
    const CreateProjectOptions& options,
    CreateProjectResult& output,
    std::string& error);

// Writes a default project.vcrproject.json inside an existing directory that
// already contains an assets/ folder (creating assets/scenes/editor_scene.json
// if no scene file exists yet).
[[nodiscard]] bool initialize_manifest_for_existing_directory(
    const std::filesystem::path& project_directory,
    std::string_view display_name,
    std::string& error);

[[nodiscard]] std::filesystem::path default_projects_root_directory();

[[nodiscard]] std::filesystem::path default_project_registry_path(
    std::string_view explicit_registry_path = {});

[[nodiscard]] std::optional<std::filesystem::path> find_sibling_executable(
    std::string_view executable_filename);

[[nodiscard]] bool launch_detached_executable(
    const std::filesystem::path& executable_path,
    std::span<const std::string> arguments,
    const std::filesystem::path& working_directory,
    std::string& error);

[[nodiscard]] bool open_directory_in_file_explorer(
    const std::filesystem::path& directory,
    std::string& error);

[[nodiscard]] std::string current_utc_iso8601();

} // namespace vcr
