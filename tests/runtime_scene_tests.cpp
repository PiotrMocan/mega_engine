#include "runtime_scene.hpp"
#include "model_lod.hpp"
#include "asset_preparation.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>

namespace {
void expect(bool result, const char *message) {
    if (!result) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}
} // namespace
int main() {
    auto vp = glm::perspective(glm::radians(65.0F), 16.0F / 9.0F, 0.1F, 100.0F);
    vp[1][1] *= -1;
    const vcr::ViewFrustum frustum(vp);
    const auto centered = vcr::lod_projection({{0, 0, -10}, {1, 1, 1}}, vp, {1920, 1080});
    const auto off_axis = vcr::lod_projection({{8, 0, -10}, {1, 1, 1}}, vp, {1920, 1080});
    expect(centered.nearest_depth == 9 && off_axis.pixels > centered.pixels,
           "LOD accounts for off-axis perspective magnification");
    expect(vcr::lod_projection({{0, 0, 0}, {1, 1, 1}}, vp, {1920, 1080}).nearest_depth == 0,
           "near crossing forces LOD0");
    expect(frustum.intersects({{0, 0, -5}, {1, 1, 1}}), "inside frustum");
    expect(!frustum.intersects({{100, 0, -5}, {1, 1, 1}}), "outside side plane");
    expect(!frustum.intersects({{0, 0, 5}, {1, 1, 1}}), "behind camera");
    expect(frustum.intersects({{0, 0, 0}, {2, 2, 2}}), "near crossing is retained");
    const auto transformed =
        vcr::transform_bounds({{0, 0, 0}, {1, 2, 3}}, {1, 2, 0, 4}, {0, -2, 0, 5}, {0, 0, 3, 6});
    expect(transformed.extent == glm::vec3(5, 4, 9) && transformed.center == glm::vec3(4, 5, 6),
           "bounds support shear and negative scale");
    vcr::OcclusionGrid grid;
    const glm::vec2 guard = glm::vec2(2) / glm::vec2(1920, 1080);
    grid.clear();
    expect(!grid.occluded({{0, 0, -10}, {.1F, .1F, .1F}}, vp, guard), "empty grid retains objects");
    grid.add_box(vcr::bounds_corners({{0, 0, -5}, {2, 2, 1}}), vp, guard);
    expect(grid.occluded({{0, 0, -10}, {.1F, .1F, .1F}}, vp, guard),
           "fully hidden object is rejected");
    expect(!grid.occluded({{0, 0, -2}, {.1F, .1F, .1F}}, vp, guard),
           "object in front remains visible");
    expect(!grid.occluded({{3, 0, -6}, {2, 2, 1}}, vp, guard),
           "partial silhouette remains visible");
    expect(!grid.occluded({{0, 0, 0}, {1, 1, 1}}, vp, guard),
           "near plane crossing remains visible");
    grid.clear();
    grid.add_box(vcr::bounds_corners({{0, 0, 0}, {2, 2, 2}}), vp, guard);
    expect(!grid.occluded({{0, 0, -10}, {.1F, .1F, .1F}}, vp, guard),
           "near clipped occluder is not approximated");
    grid.clear();
    grid.add_box(vcr::bounds_corners({{-3, 0, -5}, {1, 2, 1}}), vp, guard);
    grid.add_box(vcr::bounds_corners({{3, 0, -5}, {1, 2, 1}}), vp, guard);
    expect(!grid.occluded({{0, 0, -10}, {.1F, .1F, .1F}}, vp, guard), "gaps remain visible");
    vcr::RuntimeTransforms transforms;
    const vcr::SceneNodeId id{42, 1};
    vcr::TransformRows a, b;
    b.x.w = 2;
    expect(transforms.set(id, a), "bind runtime node");
    transforms.commit();
    expect(transforms.set(id, b), "move runtime node");
    expect(transforms.previous(id) == a && transforms.current(id) == b,
           "history belongs to last submission");
    const auto revision = transforms.revision();
    transforms.commit();
    expect(transforms.previous(id) == b && transforms.revision() > revision,
           "settle one-shot movement");
    const auto settled = transforms.revision();
    transforms.commit();
    expect(transforms.revision() == settled, "static frame is unchanged");
    expect(transforms.set({42, 2}, a) && !transforms.contains(id),
           "reused slot invalidates stale handle");
    expect(transforms.previous({42, 2}) == a, "new object has no inherited motion");
    expect(!transforms.set({vcr::max_scene_nodes, 1}, a), "out of range handle rejected");
    transforms.begin_snapshot();
    expect(!transforms.contains({42, 2}), "removed node is not active");
    expect(transforms.set({42, 2}, a) && transforms.contains({42, 2}), "retained node rebinds");
    const std::array errors{0.0F, .01F, .03F};
    expect(vcr::select_lod(errors, 1, 100, 500, 0) == 2, "distant object selects reduced mesh");
    expect(vcr::select_lod(errors, 1, 5, 500, 0) == 0, "coarsening has margin");
    expect(vcr::select_lod(errors, 1, 5, 500, 1) == 1, "hysteresis retains within budget");
    expect(vcr::select_lod(errors, 1, 4, 500, 1) == 0, "refinement respects error budget");
    vcr::SceneDocument scene;
    auto parent = scene.create_node("parent", vcr::SceneNodeKind::folder);
    auto child = scene.create_node("child", vcr::SceneNodeKind::cube, parent);
    std::vector<vcr::SceneGpuInstance> cubes;
    std::vector<vcr::SceneModelInstance> models;
    static_cast<void>(scene.build_render_view(cubes, models));
    expect(scene.last_transform_updates() == 2, "initial world transforms");
    static_cast<void>(scene.build_render_view(cubes, models));
    expect(scene.last_transform_updates() == 0, "unchanged world transforms are cached");
    scene.node(parent)->get().transform.translation.x = 5;
    static_cast<void>(scene.build_render_view(cubes, models));
    expect(scene.last_transform_updates() == 2 && cubes[0].model_row_0.w == 5,
           "parent dirtiness reaches child");
    scene.node(child)->get().material.roughness = .4F;
    static_cast<void>(scene.build_render_view(cubes, models));
    expect(scene.last_transform_updates() == 0, "material change does not recompute matrices");
    expect(scene.reparent(child, {}), "reparent succeeds");
    static_cast<void>(scene.build_render_view(cubes, models));
    expect(cubes[0].model_row_0.w == 0, "reparent invalidates cached world transform");
    vcr::CookedModelAsset model;
    constexpr unsigned side = 24;
    for (unsigned y = 0; y <= side; ++y) {
        for (unsigned x = 0; x <= side; ++x) {
            vcr::CookedModelVertex v;
            v.position = {float(x), 0, float(y)};
            v.normal = {0, 1, 0};
            v.texcoord = {float(x) / side, float(y) / side};
            model.vertices.push_back(v);
        }
    }
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            const unsigned a = y * (side + 1) + x, b = a + 1, c = a + side + 1, d = c + 1;
            for (auto i : {a, c, b, b, c, d}) {
                model.indices.push_back(i);
            }
        }
    }
    const auto original_indices = model.indices;
    model.submeshes.push_back({.index_count = model.indices.size()});
    vcr::prepare_model_lods(model);
    const auto &mesh = model.submeshes.front();
    expect(std::equal(original_indices.begin(), original_indices.end(), model.indices.begin()),
           "LOD0 is preserved exactly");
    expect(mesh.lod_count[1] > 0 && mesh.lod_count[1] < mesh.lod_count[0] &&
               mesh.lod_count[2] < mesh.lod_count[1],
           "LOD simplification reduces triangles");
    for (unsigned level = 1; level < 3; ++level) {
        expect(std::isfinite(mesh.lod_error[level]) && mesh.lod_error[level] >= 0,
               "finite geometric error");
        for (unsigned i = 0; i < mesh.lod_count[level]; ++i) {
            expect(model.indices[mesh.lod_first[level] + i] < model.vertices.size(),
                   "LOD index stays in vertex buffer");
        }
    }
    vcr::AssetPreparation worker;
    worker.start();
    expect(worker.request(std::filesystem::current_path(), "missing-a.vcrasset.json"),
           "queue asset preparation");
    expect(worker.request(std::filesystem::current_path(), "missing-b.vcrasset.json"),
           "second bounded job");
    expect(!worker.request(std::filesystem::current_path(), "missing-c.vcrasset.json"),
           "outstanding job limit includes completed results");
    unsigned completed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed < 2 && std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.poll()) {
            expect(!result->model && !result->error.empty(), "owned preparation error");
            ++completed;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expect(completed == 2 && worker.high_water() == 2, "both bounded jobs complete");
    worker.stop();
    worker.start();
    expect(worker.request(std::filesystem::current_path(), "cancel.vcrasset.json"),
           "restart worker");
    worker.stop();
    worker.stop();
    std::puts("Runtime history, conservative visibility and LOD tests passed.");
}
