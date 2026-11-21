#include "sublevel_streaming.hpp"

#include "project_system.hpp"
#include "scene_io.hpp"

#include <map>
#include <stdexcept>

namespace vcr {
namespace {
using UuidMap = std::map<SceneUuid, SceneUuid>;

void namespace_records(std::vector<SceneNodeRecord>& records, std::string_view path) {
    std::uint64_t seed = 14695981039346656037ULL;
    for (const unsigned char c : path)
        seed = (seed ^ c) * 1099511628211ULL;
    UuidMap mapping;
    for (auto& record : records) {
        const auto uuid = deterministic_scene_uuid(seed ^ record.uuid.high, record.uuid.low);
        mapping.emplace(record.uuid, uuid);
        record.uuid = uuid;
    }
    const auto remap = [&](SceneUuid& id) {
        if (const auto it = mapping.find(id); it != mapping.end())
            id = it->second;
    };
    for (auto& record : records) {
        if (record.gameplay)
            remap(record.gameplay->target_node_uuid);
        if (record.off_mesh_link)
            remap(record.off_mesh_link->target_node_uuid);
        if (record.npc_ai) {
            remap(record.npc_ai->target_node_uuid);
            remap(record.npc_ai->patrol_spline_uuid);
        }
    }
}

SceneNodeId attach(SceneDocument& scene, std::vector<SceneNodeRecord>& records, const std::string& name) {
    if (records.size() >= max_scene_nodes - scene.size())
        return {};
    SceneDocument staged = scene;
    const auto root = staged.create_node(name.size() <= max_scene_node_name_bytes ? name : "Sublevel",
                                         SceneNodeKind::folder);
    if (!root)
        return {};
    std::vector<SceneNodeId> ids;
    ids.reserve(records.size());
    for (auto& record : records) {
        const auto parent = record.parent_record_index == invalid_scene_node_index
                                ? root
                                : ids.at(record.parent_record_index);
        const auto id =
            staged.create_node_with_uuid(record.value.name, record.value.kind, record.uuid, parent);
        if (!id)
            return {};
        staged.node(id)->get() = std::move(record.value);
        if (record.collider && !staged.set_collider(id, *record.collider))
            return {};
        if (record.rigid_body && !staged.set_rigid_body(id, *record.rigid_body))
            return {};
        if (record.prefab_instance && !staged.set_prefab_instance(id, *record.prefab_instance))
            return {};
        if (record.audio_source && !staged.set_audio_source(id, *record.audio_source))
            return {};
        if (record.audio_listener && !staged.set_audio_listener(id, *record.audio_listener))
            return {};
        if (record.reverb_zone && !staged.set_reverb_zone(id, *record.reverb_zone))
            return {};
        if (record.animator && !staged.set_animator(id, *record.animator))
            return {};
        if (record.gameplay && !staged.set_gameplay(id, *record.gameplay))
            return {};
        if (record.ui_marker && !staged.set_ui_marker(id, *record.ui_marker))
            return {};
        if (record.spline && !staged.set_spline(id, *record.spline))
            return {};
        if (record.nav_obstacle && !staged.set_nav_obstacle(id, *record.nav_obstacle))
            return {};
        if (record.off_mesh_link && !staged.set_off_mesh_link(id, *record.off_mesh_link))
            return {};
        if (record.npc_ai && !staged.set_npc_ai(id, *record.npc_ai))
            return {};
        ids.push_back(id);
    }
    scene = std::move(staged);
    return root;
}
} // namespace

SublevelStreaming::~SublevelStreaming() { stop(); }

void SublevelStreaming::stop() noexcept {
    if (worker_.joinable()) {
        {
            std::lock_guard lock(mutex_);
            worker_.request_stop();
        }
        wake_.notify_one();
        worker_.join();
    }
    request_.reset();
    result_.reset();
    ready_.store(false, std::memory_order_relaxed);
    busy_ = false;
    levels_.clear();
}

void SublevelStreaming::configure(const std::filesystem::path& asset_root,
                                  std::span<const SublevelReference> references) {
    stop();
    asset_root_ = asset_root;
    if (references.empty())
        return;
    if (references.size() > max_sublevels)
        throw std::length_error("Too many sublevels");
    for (const auto& ref : references)
        levels_.push_back(Level{ref});
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

void SublevelStreaming::run(std::stop_token token) {
    for (;;) {
        Result result;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return token.stop_requested() || request_.has_value(); });
            if (token.stop_requested())
                return;
            result.index = *request_;
            request_.reset();
        }
        try {
            const auto& ref = levels_[result.index].reference;
            const auto base = std::filesystem::canonical(asset_root_);
            const auto file = std::filesystem::canonical(
                base / std::filesystem::path(std::u8string(ref.scene_path.begin(), ref.scene_path.end())));
            const auto relative = file.lexically_relative(base);
            SceneDocument loaded;
            if (!valid_project_scene_relative_path(ref.scene_path) || relative.empty() ||
                *relative.begin() == "..") {
                result.error = "Sublevel outside asset directory: " + ref.scene_path;
            } else if (load_scene_file(file, loaded, result.error)) {
                if (!loaded.sublevels.empty())
                    result.error = "Nested sublevels are unsupported: " + ref.scene_path;
                else {
                    loaded.build_node_records(result.records);
                    namespace_records(result.records, ref.scene_path);
                }
            }
        } catch (const std::exception& ex) {
            result.error = ex.what();
        }
        std::lock_guard lock(mutex_);
        result_.emplace(std::move(result));
        ready_.store(true, std::memory_order_release);
    }
}

bool SublevelStreaming::update(SceneDocument& scene, glm::vec3 camera, std::string& error) {
    bool changed = false;
    const auto in_range = [&](const Level& level, float factor) {
        const auto delta = camera - level.reference.center;
        const float radius = level.reference.radius * factor;
        return glm::dot(delta, delta) <= radius * radius;
    };
    for (auto& level : levels_) {
        if (level.root && !in_range(level, 1.25F)) {
            changed |= scene.destroy_subtree(level.root);
            level.root = {};
        }
    }
    if (ready_.load(std::memory_order_acquire)) {
        Result result;
        {
            std::lock_guard lock(mutex_);
            result = std::move(*result_);
            result_.reset();
            ready_.store(false, std::memory_order_relaxed);
        }
        busy_ = false;
        auto& level = levels_[result.index];
        if (in_range(level, 1.F)) {
            if (result.error.empty()) {
                level.root = attach(scene, result.records, level.reference.scene_path);
                if (!level.root)
                    result.error = "Unable to attach sublevel: " + level.reference.scene_path;
                else
                    changed = true;
            }
            if (!result.error.empty()) {
                error = std::move(result.error);
                level.failed = true;
            }
        }
    }
    if (!busy_) {
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            auto& level = levels_[i];
            if (!in_range(level, 1.F)) {
                level.failed = false;
                continue;
            }
            if (level.root || level.failed)
                continue;
            {
                std::lock_guard lock(mutex_);
                request_ = i;
                busy_ = true;
            }
            wake_.notify_one();
            break;
        }
    }
    return changed;
}
} // namespace vcr
