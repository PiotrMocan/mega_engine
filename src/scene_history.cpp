#include "scene_history.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace vcr {
SceneHistoryDelta::SceneHistoryDelta(const SceneDocument& before, const SceneDocument& after) {
    using Doc = SceneDocument;
    const auto remember = [&](auto member) {
        changes_.emplace_back([member, previous = before.*member](Doc& doc) { doc.*member = previous; });
    };
    // Small document metadata includes the saved revisions used by dirty-state tracking.
    if (before.sublevels != after.sublevels)
        remember(&Doc::sublevels);
    remember(&Doc::startup_camera);
    remember(&Doc::lighting);
    remember(&Doc::tone_mapper);
    remember(&Doc::lod_fade);
    remember(&Doc::exposure);
    remember(&Doc::subsurface);
    remember(&Doc::fog);
    remember(&Doc::content_revision_);
    remember(&Doc::structure_revision_);
    remember(&Doc::first_root_);
    remember(&Doc::last_root_);
    remember(&Doc::live_count_);
    remember(&Doc::uuid_tombstones_);

    const auto vector_delta = [&](auto member, auto equal) {
        const auto& old = before.*member;
        const auto& now = after.*member;
        using Item = typename std::decay_t<decltype(old)>::value_type;
        std::vector<std::pair<std::size_t, Item>> edits;
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (i >= now.size() || !equal(old[i], now[i]))
                edits.emplace_back(i, old[i]);
        }
        if (edits.empty() && old.size() == now.size())
            return;
        changes_.emplace_back([member, size = old.size(), edits = std::move(edits)](Doc& doc) {
            auto& target = doc.*member;
            target.resize(size);
            for (const auto& [index, value] : edits)
                target[index] = value;
        });
    };
    const auto links_and_value = [](const Doc::Slot& s) {
        return std::tie(s.uuid, s.value, s.generation, s.parent, s.first_child, s.last_child,
                        s.previous_sibling, s.next_sibling, s.alive);
    };
    vector_delta(&Doc::slots_,
                 [&](const auto& a, const auto& b) { return links_and_value(a) == links_and_value(b); });
    vector_delta(&Doc::free_indices_, std::equal_to<>{});
    vector_delta(&Doc::uuid_buckets_, std::equal_to<>{});

    const auto components = [&](auto member) {
        const auto& old = before.*member;
        const auto& now = after.*member;
        // Structural changes must restore dense iteration order as well as values.
        if (!std::ranges::equal(old.owners(), now.owners())) {
            remember(member);
            return;
        }
        using Value = typename decltype(old.values())::value_type;
        std::vector<std::pair<SceneNodeId, Value>> edits;
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (old.values()[i] != now.values()[i])
                edits.emplace_back(old.owners()[i], old.values()[i]);
        }
        if (!edits.empty())
            changes_.emplace_back([member, edits = std::move(edits)](Doc& doc) {
                for (const auto& [id, value] : edits)
                    (doc.*member).get_mut(id)->get() = value;
            });
    };
    components(&Doc::colliders_);
    components(&Doc::rigid_bodies_);
    components(&Doc::prefab_instances_);
    components(&Doc::audio_sources_);
    components(&Doc::audio_listeners_);
    components(&Doc::reverb_zones_);
    components(&Doc::animators_);
    components(&Doc::gameplays_);
    components(&Doc::ui_markers_);
    components(&Doc::splines_);
    components(&Doc::nav_obstacles_);
    components(&Doc::off_mesh_links_);
    components(&Doc::npc_ais_);
}

SceneDocument SceneHistoryDelta::restore(const SceneDocument& current) const {
    SceneDocument restored(current);
    for (const auto& change : changes_)
        change(restored);
    for (auto& slot : restored.slots_)
        slot.world_revision = 0;
    return restored;
}
} // namespace vcr
