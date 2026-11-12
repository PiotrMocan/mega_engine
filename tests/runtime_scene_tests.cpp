#include "runtime_scene.hpp"
#include "bounds_hierarchy.hpp"
#include "camera_projection.hpp"
#include "model_lod.hpp"
#include "gpu_grass.hpp"
#include "asset_preparation.hpp"
#include <filesystem>
#include <fstream>
#include "opacity_micromap.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
void expect(bool result, const char *message) {
    if (!result) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

void lod_transform_scale_tests() {
    const auto bound = [](const glm::mat4 &matrix) {
        return vcr::lod_transform_scale(
            {matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]},
            {matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]},
            {matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2]});
    };
    expect(bound(glm::mat4(0)) == 0, "collapsed transform has zero geometric error");
    const auto rotation = glm::rotate(glm::mat4(1), .73F, glm::normalize(glm::vec3(1, 2, 3)));
    for (const auto scale : {glm::vec3(1), glm::vec3(3), glm::vec3(-2, .5F, 4),
                             glm::vec3(0, 1, 2), glm::vec3(1.0e-8F), glm::vec3(1.0e8F)}) {
        auto matrix = glm::scale(rotation, scale);
        matrix[3] = glm::vec4(200, -5, 43, 1); // Translation cannot affect stretch.
        const double maximum = std::max({std::abs(double(scale.x)), std::abs(double(scale.y)),
                                          std::abs(double(scale.z))});
        const double result = bound(matrix);
        expect(result >= maximum && result <= maximum * 1.00001,
            "rotated and reflected TRS uses the largest axis scale without sqrt(3) inflation");
    }
    // Deterministic arbitrary matrices cover shear from composed parent TRS,
    // near singular transforms and reflections. Check actual transformed unit
    // directions in double precision, independently of the norm formula.
    std::uint32_t seed = 0x7931abcU;
    const auto random = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return float(seed >> 8U) / float(0x00ffffffU) * 2.0F - 1.0F;
    };
    for (unsigned test = 0; test < 256; ++test) {
        glm::mat4 matrix(1);
        double frobenius_squared = 0;
        for (unsigned c = 0; c < 3; ++c) {
            for (unsigned r = 0; r < 3; ++r) {
                matrix[c][r] = random() * 10.0F;
                frobenius_squared += double(matrix[c][r]) * matrix[c][r];
            }
        }
        const double result = bound(matrix);
        expect(result <= std::sqrt(frobenius_squared) * 1.00001,
            "shear bound is no looser than padded Frobenius");
        for (unsigned direction = 0; direction < 128; ++direction) {
            const glm::dvec3 vector = glm::normalize(glm::dvec3(random(), random(), random()));
            expect(glm::length(glm::dmat3(matrix) * vector) <= result,
                "LOD error scale conservatively bounds sheared displacement");
        }
    }
    auto overflow = glm::mat4(1);
    overflow[0][0] = std::numeric_limits<float>::max();
    expect(std::isinf(bound(overflow)), "unrepresentable stretch retains full detail");
}

void bounds_hierarchy_tests() {
    constexpr std::size_t capacity = 4096;
    vcr::BoundsHierarchy hierarchy;
    const auto projection = vcr::camera_projection(16.0F / 9.0F, .1F, 500.0F);
    const vcr::ViewFrustum default_frustum(projection);
    std::array<std::uint8_t, 3> failure_mask{};
    expect(!hierarchy.query(default_frustum, failure_mask) &&
               std::ranges::all_of(failure_mask, [](auto value) { return value == 1; }),
           "unbuilt hierarchy fails with a fully visible fallback mask");
    expect(hierarchy.build({}) && hierarchy.refit({}) && hierarchy.query(default_frustum, {}) &&
               hierarchy.valid() && hierarchy.size() == 0 && hierarchy.node_count() == 0,
           "empty hierarchy is safe without allocation or capacity preparation");
    expect(hierarchy.prepare_capacity(capacity) && hierarchy.capacity() == capacity,
           "hierarchy prepares bounded storage explicitly");
    const auto reserved = hierarchy.reserved_bytes();
    expect(hierarchy.prepare_capacity(8) && hierarchy.reserved_bytes() == reserved &&
               hierarchy.capacity() == capacity,
           "smaller capacity requests preserve prepared storage");

    std::vector<vcr::Bounds> bounds;
    bounds.reserve(capacity);
    std::uint32_t random_state = 0x3A91E742U;
    const auto random = [&random_state](float low, float high) {
        random_state = random_state * 1664525U + 1013904223U;
        return low + (high - low) * static_cast<float>(random_state >> 8) / 16777216.0F;
    };
    for (std::size_t i = 0; i < capacity; ++i) {
        vcr::Bounds bound{{random(-800, 800), random(-150, 150), random(-700, 100)},
                          {random(0, 20), random(0, 15), random(0, 20)}};
        if (i % 11 == 0) {
            // Coincident centroids, overlapping and enclosing objects defeat
            // assumptions that source order already provides spatial separation.
            bound = {{0, 0, -12}, glm::vec3(static_cast<float>(i % 37))};
        } else if (i % 13 == 0) {
            bound = {{random(-.2F, .2F), random(-.2F, .2F), random(-.12F, .02F)},
                      {random(0, .1F), random(0, .1F), random(0, .1F)}};
        } else if (i % 17 == 0) {
            bound = vcr::transform_bounds({{1, -2, 3}, {2, 4, 1}},
                {1, 2, -.3F, bound.center.x}, {0, -2, .7F, bound.center.y},
                {-.2F, .6F, 3, bound.center.z});
        }
        bounds.push_back(bound);
    }
    // Finite inputs whose float c +/- extent or plane products can overflow.
    // Parent pruning must preserve even the scalar predicate's NaN retention.
    const auto huge = std::numeric_limits<float>::max();
    bounds[5] = {{huge, 0, -huge}, {huge, 1, huge}};
    bounds[6] = {{1.0e30F, -1.0e30F, -1.0e30F}, {1.0e23F, 1, 1}};
    bounds[7] = {{1, 0, -2}, {std::numeric_limits<float>::denorm_min(), 0, 0}};

    std::array<glm::mat4, 8> cameras{};
    cameras[0] = projection;
    cameras[1] = projection * glm::lookAt(glm::vec3(13, 4, 12), glm::vec3(-3, 1, -40), glm::vec3(0, 1, 0));
    cameras[2] = projection * glm::lookAt(glm::vec3(-60, 90, -30), glm::vec3(0, 0, -120), glm::vec3(0, 1, 0));
    cameras[3] = vcr::camera_projection(.75F, .01F, 1200.0F);
    cameras[3][1][1] *= -1;
    cameras[4] = projection;
    cameras[4][2][0] += .001F;
    cameras[4][2][1] -= .002F;
    cameras[5] = glm::mat4(0.0F);
    cameras[6] = projection;
    cameras[6][0][0] = std::numeric_limits<float>::quiet_NaN();
    cameras[7] = glm::mat4(huge);

    std::vector<std::uint8_t> mask(capacity, 0xA5);
    const auto matches_scalar = [&](std::span<const vcr::Bounds> source, const glm::mat4& vp) {
        std::fill(mask.begin(), mask.end(), std::uint8_t{0xA5});
        const auto output = std::span(mask).first(source.size());
        const vcr::ViewFrustum frustum(vp);
        expect(hierarchy.query(frustum, output), "valid hierarchy query succeeds");
        for (std::size_t i = 0; i < source.size(); ++i) {
            expect(output[i] == (frustum.intersects(source[i]) ? 1U : 0U),
                   "hierarchy writes the exact scalar visibility mask in source order");
        }
    };
    for (const std::size_t count : {0U, 1U, 7U, 8U, 9U, 16U, 17U, 63U, 64U, 65U, 4096U}) {
        const auto source = std::span<const vcr::Bounds>(bounds).first(count);
        expect(hierarchy.build(source) && hierarchy.size() == count &&
                   hierarchy.reserved_bytes() == reserved,
               "build uses prepared capacity at leaf and balanced-tree boundaries");
        unsigned depth_bound = 0;
        std::size_t width = vcr::BoundsHierarchy::leaf_capacity;
        while (width < count) { width *= 2; ++depth_bound; }
        expect(hierarchy.maximum_depth() <= depth_bound &&
                   hierarchy.node_count() <= (count == 0 ? 0 : 2 * count - 1),
               "median hierarchy stays balanced and bounded even for coincident centroids");
        std::array<bool, capacity> seen{};
        for (const auto source_index : hierarchy.source_indices()) {
            expect(source_index < count && !seen[source_index],
                   "hierarchy source indices remain a duplicate-free permutation");
            seen[source_index] = true;
        }
        expect(hierarchy.source_indices().size() == count,
               "hierarchy includes every source exactly once");
        for (const auto& camera : cameras) matches_scalar(source, camera);
    }
    const std::vector<std::uint32_t> original_indices(hierarchy.source_indices().begin(),
                                                      hierarchy.source_indices().end());
    const auto original_nodes = hierarchy.node_count();
    const auto original_depth = hierarchy.maximum_depth();
    expect(hierarchy.build(bounds) && std::ranges::equal(original_indices, hierarchy.source_indices()),
           "rebuilding identical input uses deterministic centroid and source-index ties");
    for (unsigned step = 0; step < 3; ++step) {
        for (std::size_t i = 8; i < bounds.size(); ++i) {
            bounds[i].center += glm::vec3(random(-60, 60), random(-10, 10), random(-30, 30));
            bounds[i].extent *= random(.6F, 1.4F);
        }
        expect(hierarchy.refit(bounds) && hierarchy.node_count() == original_nodes &&
                   hierarchy.maximum_depth() == original_depth &&
                   std::ranges::equal(original_indices, hierarchy.source_indices()),
               "refit updates moved bounds without rebuilding topology or source identities");
        for (const auto& camera : cameras) matches_scalar(bounds, camera);
        expect(hierarchy.reserved_bytes() == reserved,
               "refit and query retain every prepared allocation capacity");
    }

    // Narrow, oblique slabs near large coordinates stress cancellation in the
    // scalar float predicate. A geometric-only parent test is insufficient here.
    auto slab = glm::mat4(0.0F);
    slab[0][0] = .731237F;
    slab[1][0] = -.391237F;
    slab[2][0] = .192731F;
    slab[3][0] = -1.0e12F;
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const float y = random(-1.0e18F, 1.0e18F);
        const float z = random(-1.0e18F, 1.0e18F);
        const float x = -(slab[1][0] * y + slab[2][0] * z + slab[3][0]) / slab[0][0];
        const auto direction = i % 2 ? std::numeric_limits<float>::infinity()
                                     : -std::numeric_limits<float>::infinity();
        bounds[i] = {{std::nextafter(x, direction), y, z}, glm::vec3(random(0, 1.0e10F))};
    }
    expect(hierarchy.build(bounds), "oblique boundary stress fixture builds");
    matches_scalar(bounds, slab);

    vcr::BoundsHierarchy moved(std::move(hierarchy));
    expect(moved.valid() && moved.size() == capacity && moved.reserved_bytes() == reserved &&
               !hierarchy.valid() && hierarchy.capacity() == 0 &&
               !hierarchy.build(std::span<const vcr::Bounds>(bounds).first(1)),
           "move transfers prepared ownership without leaving allocating moved-from hot paths");
    hierarchy = std::move(moved);
    expect(!hierarchy.prepare_capacity(std::numeric_limits<std::size_t>::max()) && hierarchy.valid(),
           "unsupported capacity is rejected without damaging the existing tree");
    matches_scalar(bounds, slab);
    failure_mask.fill(0xA5);
    expect(!hierarchy.query(default_frustum, failure_mask) &&
               std::ranges::all_of(failure_mask, [](auto value) { return value == 1; }),
           "wrong-sized output fails and overwrites its entire range with visible");
    expect(!hierarchy.refit(std::span<const vcr::Bounds>(bounds).first(8)) && !hierarchy.valid(),
           "source-count-changing refit invalidates stale hierarchy state");

    const std::array<vcr::Bounds, 4> invalid{{
        {{std::numeric_limits<float>::quiet_NaN(), 0, -1}, {1, 1, 1}},
        {{0, 0, -std::numeric_limits<float>::infinity()}, {1, 1, 1}},
        {{0, 0, -1}, {1, std::numeric_limits<float>::infinity(), 1}},
        {{0, 0, -1}, {1, -1, 1}}
    }};
    for (const auto& bad : invalid) {
        const std::array source{bad};
        expect(!hierarchy.build(source) && !hierarchy.valid() &&
                   !hierarchy.query(default_frustum, failure_mask) &&
                   std::ranges::all_of(failure_mask, [](auto value) { return value == 1; }),
               "invalid bounds fail explicitly and cannot silently false-cull");
        const std::array<vcr::Bounds, 1> valid{{{{0, 0, -5}, {1, 1, 1}}}};
        expect(hierarchy.build(valid) && !hierarchy.refit(source) && !hierarchy.valid(),
               "invalid refit cannot leave queryable stale bounds");
    }
    vcr::BoundsHierarchy limited;
    expect(limited.prepare_capacity(8) &&
               !limited.build(std::span<const vcr::Bounds>(bounds).first(9)) && !limited.valid(),
           "oversized build fails before any unprepared allocation");
    std::array<std::uint8_t, 8> small_mask{};
    expect(limited.build(std::span<const vcr::Bounds>(bounds).first(8)) &&
               limited.prepare_capacity(16) && limited.valid() && limited.size() == 8 &&
               limited.query(vcr::ViewFrustum(slab), small_mask),
           "cold capacity growth preserves an already built hierarchy");
    for (std::size_t i = 0; i < small_mask.size(); ++i) {
        expect(small_mask[i] == (vcr::ViewFrustum(slab).intersects(bounds[i]) ? 1U : 0U),
               "capacity growth retains exact source bounds and query results");
    }
    expect(hierarchy.build({}) && hierarchy.query(default_frustum, {}) &&
               hierarchy.reserved_bytes() == reserved,
           "empty rebuild resets populated state while retaining bounded storage");
}

void authored_lod_tests() {
    vcr::CookedModelAsset base;
    base.materials = {{.name = "metal"}, {.name = "detail"}};
    base.vertices.resize(4);
    base.vertices[0].normal = {0, 1, 0};
    base.vertices[0].texcoord = {.25F, .75F};
    base.vertices[0].tangent = {1, 0, 0, -1};
    base.indices = {0, 1, 2, 1, 2, 3, 0, 2, 3};
    base.submeshes = {{.first_index = 0, .index_count = 3, .material_index = 0},
                      {.first_index = 3, .index_count = 3, .material_index = 1},
                      {.first_index = 6, .index_count = 3, .material_index = 0}};
    base.lods = {{"lod1.vcrasset.json", .05F}, {"lod2.vcrasset.json", .2F}};
    std::array<vcr::CookedModelAsset, 2> levels;
    // Reverse material order in LOD1; omit the detail material in LOD2.
    levels[0].materials = {{.name = "detail"}, {.name = "metal"}};
    levels[0].vertices = base.vertices;
    levels[0].vertices[0].position = {0, .05F, 0};
    levels[0].indices = {0, 2, 3, 0, 1, 2};
    levels[0].submeshes = {{.index_count = 3, .material_index = 0},
                           {.first_index = 3, .index_count = 3, .material_index = 1}};
    levels[1].materials = {{.name = "metal"}};
    levels[1].vertices = base.vertices;
    levels[1].indices = {1, 2, 3};
    levels[1].submeshes = {{.index_count = 3}};
    auto prepared = base;
    vcr::prepare_authored_model_lods(prepared, levels, 4096);
    expect(prepared.vertices.size() < 12 && prepared.indices.size() == 21 &&
               prepared.submeshes.size() == 2, "authored geometry aggregates per material");
    const auto &metal = prepared.submeshes[0];
    const auto &detail = prepared.submeshes[1];
    expect(metal.lod_count == std::array<std::uint32_t, 4>{6, 3, 3, 3} && metal.lod_error[3] > metal.lod_error[2] &&
               detail.lod_count == std::array<std::uint32_t, 4>{3, 3, 0, 0} && std::isfinite(detail.lod_error[3]),
           "legacy authored assets receive a fourth LOD while preserving intentional material removal");
    const auto &shifted = prepared.vertices[prepared.indices[metal.lod_first[1]]];
    expect(shifted.position[1] == .05F &&
               prepared.vertices[prepared.indices[metal.lod_first[2]]].position == levels[1].vertices[1].position &&
               prepared.vertices[prepared.indices[detail.lod_first[1]+1]].position == levels[0].vertices[2].position,
           "authored LOD maps material names and preserves indexed positions after lossless sharing");
    expect(shifted.normal == base.vertices[0].normal &&
               shifted.texcoord == base.vertices[0].texcoord && shifted.tangent == base.vertices[0].tangent,
           "shared authored vertices preserve normal, UV and tangent seams exactly");
    const auto removed_level = vcr::select_lod(detail.lod_error, 1, 1000, 500, 0);
    expect(removed_level >= 2 && detail.lod_count[removed_level] == 0,
           "finite error selects authored removal using existing pixel criterion");
    for (const auto &mesh : prepared.submeshes) {
        for (unsigned level = 0; level < vcr::model_lod_count; ++level) {
            expect(mesh.lod_first[level] + mesh.lod_count[level] <= prepared.indices.size(),
                   "every runtime authored range remains inside combined index buffer");
        }
    }
    const auto rejects = [&](auto invalid_base, const auto &invalid_levels, std::size_t budget) {
        try {
            vcr::prepare_authored_model_lods(invalid_base, invalid_levels, budget);
        } catch (const std::exception &) {
            return true;
        }
        return false;
    };
    expect(rejects(base, levels, 64), "authored combined upload budget is enforced");
    {
        auto four_level = base;
        four_level.lods.push_back({"lod3.vcrasset.json", .4F});
        const std::array full_chain{levels[0], levels[1], levels[1]};
        vcr::prepare_authored_model_lods(four_level, full_chain, 4096);
        expect(four_level.submeshes[0].lod_count == std::array<std::uint32_t, 4>{6,3,3,3} &&
                   four_level.submeshes[0].lod_error[3] == .4F,
               "explicit LOD0..LOD3 chain loads all four authored ranges");
    }
    auto bad_levels = levels;
    bad_levels[1].materials[0].name = "unknown";
    expect(rejects(base, bad_levels, 4096), "unknown LOD material is rejected");
    bad_levels = levels;
    bad_levels[0].materials[1].name = "detail";
    expect(rejects(base, bad_levels, 4096), "ambiguous LOD material is rejected");
    bad_levels = levels;
    bad_levels[1].indices[0] = 99;
    expect(rejects(base, bad_levels, 4096), "out of bounds LOD index is rejected");
    bad_levels = levels;
    bad_levels[1].lods = base.lods;
    expect(rejects(base, bad_levels, 4096), "nested authored LOD is rejected without recursion");
    base.lods[1].geometric_error = .01F;
    expect(rejects(base, levels, 4096), "decreasing authored error is rejected");
}
} // namespace
void geometry_optimization_tests() {
    vcr::CookedModelAsset mesh;
    // Two triangles share positions but one has an authored UV seam. Include
    // exact duplicates and an unused vertex to exercise lossless remapping.
    for (unsigned i = 0; i < 7; ++i) {
        vcr::CookedModelVertex v{};
        v.position = {float(i % 3), float(i % 2), 0.0F};
        v.normal = {0, 0, 1};
        v.tangent = {1, 0, 0, 1};
        mesh.vertices.push_back(v);
    }
    mesh.vertices[3] = mesh.vertices[0];
    mesh.vertices[4] = mesh.vertices[1];
    mesh.vertices[5] = mesh.vertices[2];
    mesh.vertices[5].texcoord = {0.5F, 0.75F};
    mesh.indices = {0, 1, 2, 3, 4, 5, 3, 4, 5};
    vcr::CookedModelSubmesh part{};
    part.index_count = 6;
    part.lod_first = {0, 6, 6, 6};
    part.lod_count = {6, 3, 3, 3};
    part.lod_error = {0, 0.1F, 0.2F, 0.3F};
    mesh.submeshes.push_back(part);
    const auto original = mesh;
    vcr::optimize_model_geometry(mesh);
    expect(mesh.vertices.size() == 4, "geometry optimization shares only identical attribute tuples");
    expect(mesh.indices.size() == original.indices.size() && mesh.submeshes[0].lod_error == part.lod_error &&
        mesh.submeshes[0].lod_count == part.lod_count && mesh.submeshes[0].lod_first == part.lod_first,
        "geometry optimization preserves all LOD errors and draw ranges");
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        // This tiny fixture retains triangle order; the comparison checks every
        // attribute, including winding, rather than just bounding boxes.
        const auto &before = original.vertices[original.indices[i]];
        const auto &after = mesh.vertices[mesh.indices[i]];
        expect(before.position == after.position && before.normal == after.normal &&
            before.texcoord == after.texcoord && before.tangent == after.tangent,
            "geometry optimization preserves indexed surface attributes");
    }
    expect(mesh.indices[6] == 0 && mesh.indices[7] == 1 && mesh.indices[8] == 2,
        "coarsest LOD vertices occupy the first contiguous fetch range");
    auto invalid = original;
    invalid.indices[0] = 100;
    bool rejected = false;
    try { vcr::optimize_model_geometry(invalid); } catch (const std::invalid_argument &) { rejected = true; }
    expect(rejected, "geometry optimization rejects out-of-bounds vertices");
}

int main() {
    lod_transform_scale_tests();
    {
        vcr::CookedModelAsset grass;
        grass.materials.emplace_back().factors.model = vcr::MaterialModel::gpu_grass;
        for (unsigned blade = 0; blade < 32; ++blade) {
            for (unsigned corner = 0; corner < 3; ++corner) {
                const float height = corner == 1 ? 1.0F : 0.0F;
                const auto sample = vcr::evaluate_gpu_grass_blade(blade, float(corner) * .5F, height);
                vcr::CookedModelVertex vertex{};
                vertex.position = {sample.position.x, sample.position.y, sample.position.z};
                vertex.texcoord = vcr::encode_gpu_grass_uv(blade, static_cast<vcr::GpuGrassCorner>(corner), height);
                grass.indices.push_back(static_cast<unsigned>(grass.vertices.size()));
                grass.vertices.push_back(vertex);
            }
        }
        grass.submeshes.push_back({.index_count = 96});
        vcr::prepare_model_lods(grass);
        vcr::optimize_model_geometry(grass);
        std::string error;
        expect(grass.submeshes[0].lod_count == std::array<std::uint32_t, 4>{96,96,96,96} &&
            vcr::validate_gpu_grass_submesh(grass.vertices, grass.indices, grass.submeshes[0], error),
            "generic LOD preparation preserves procedural grass encoding across remapping");
    }
    geometry_optimization_tests();
    bounds_hierarchy_tests();
    authored_lod_tests();
    auto vp = vcr::camera_projection(16.0F / 9.0F, 0.1F, 100.0F);
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
    expect(!frustum.intersects({{0, 0, -102}, {.1F, .1F, .1F}}), "beyond reverse-Z far plane");
    expect(!frustum.intersects({{0, 0, -.02F}, {.001F, .001F, .001F}}),
           "before reverse-Z near plane");
    expect(frustum.intersects({{0, 0, 0}, {2, 2, 2}}), "near crossing is retained");
    const auto transformed =
        vcr::transform_bounds({{0, 0, 0}, {1, 2, 3}}, {1, 2, 0, 4}, {0, -2, 0, 5}, {0, 0, 3, 6});
    expect(transformed.extent == glm::vec3(5, 4, 9) && transformed.center == glm::vec3(4, 5, 6),
           "bounds support shear and negative scale");
    const auto ray_hit = vcr::ray_bounds_intersection(
        {0, 0, 0}, {0, 0, -1}, {{0, 0, -5}, {1, 2, 1}});
    expect(ray_hit && std::abs(*ray_hit - 4.0F) < 0.0001F,
           "editor picking ray returns nearest bounds entry");
    expect(!vcr::ray_bounds_intersection(
               {3, 0, 0}, {0, 0, -1}, {{0, 0, -5}, {1, 2, 1}}),
           "parallel picking ray rejects a missed bounds slab");
    const auto inside_hit = vcr::ray_bounds_intersection(
        {0, 0, -5}, {1, 0, 0}, {{0, 0, -5}, {1, 2, 1}});
    expect(inside_hit && *inside_hit == 0.0F,
           "picking from inside bounds starts at zero distance");
    vcr::OcclusionGrid grid;
    const glm::vec2 guard = glm::vec2(2) / glm::vec2(1920, 1080);
    grid.clear();
    expect(!grid.occluded({{0, 0, -10}, {.1F, .1F, .1F}}, vp, guard), "empty grid retains objects");
    expect(!grid.has_coverage(), "empty reverse-Z grid has no coverage");
    grid.add_box(vcr::bounds_corners({{0, 0, -5}, {2, 2, 1}}), vp, guard);
    expect(grid.has_coverage(), "reverse-Z occluder covers cells");
    expect(grid.occluded({{0, 0, -10}, {.1F, .1F, .1F}}, vp, guard),
           "fully hidden object is rejected");
    expect(!grid.occluded({{0, 0, -2}, {.1F, .1F, .1F}}, vp, guard),
           "object in front remains visible");
    expect(!grid.occluded({{3, 0, -6}, {2, 2, 1}}, vp, guard),
           "partial silhouette remains visible");
    expect(!grid.occluded({{0, 0, 0}, {1, 1, 1}}, vp, guard),
           "near plane crossing remains visible");
    expect(!grid.occluded({{0, 0, -.1F}, {.001F, .001F, .02F}}, vp, guard),
           "positive-w near clipping remains conservative");
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
    auto snapped_transform = b;
    snapped_transform.y.w = -3;
    expect(transforms.set(id, snapped_transform) && transforms.moving(),
           "second movement is pending");
    const auto pre_snap_revision = transforms.revision();
    transforms.snap_history();
    expect(transforms.previous(id) == snapped_transform &&
               transforms.current(id) == snapped_transform &&
               !transforms.moving() && transforms.revision() > pre_snap_revision,
           "history snap removes stale motion after a temporal reset");
    expect(transforms.set({42, 2}, a) && !transforms.contains(id),
           "reused slot invalidates stale handle");
    expect(transforms.previous({42, 2}) == a, "new object has no inherited motion");
    expect(!transforms.set({vcr::max_scene_nodes, 1}, a), "out of range handle rejected");
    transforms.begin_snapshot();
    expect(!transforms.contains({42, 2}), "removed node is not active");
    expect(transforms.set({42, 2}, a) && transforms.contains({42, 2}), "retained node rebinds");
    const std::array errors{0.0F, .01F, .03F, .08F};
    expect(vcr::select_lod(errors, 1, 100, 500, 0) == 3 && vcr::select_lod(errors, 1, 35, 500, 0) == 2, "distant object selects reduced mesh");
    expect(vcr::select_lod(errors, 1, 5, 500, 0) == 0, "coarsening has margin");
    expect(vcr::select_lod(errors, 1, 5, 500, 1) == 1, "hysteresis retains within budget");
    expect(vcr::select_lod(errors, 1, 4, 500, 1) == 0, "refinement respects error budget");
    vcr::SceneDocument scene;
    auto parent = scene.create_node("parent", vcr::SceneNodeKind::folder);
    auto child = scene.create_node("child", vcr::SceneNodeKind::cube, parent);
    std::vector<vcr::SceneGpuInstance> cubes;
    std::vector<vcr::SceneModelInstance> models;
    std::vector<vcr::ScenePunctualLight> lights;
    static_cast<void>(scene.build_render_view(cubes, models, lights));
    expect(scene.last_transform_updates() == 2, "initial world transforms");
    static_cast<void>(scene.build_render_view(cubes, models, lights));
    expect(scene.last_transform_updates() == 0, "unchanged world transforms are cached");
    scene.node(parent)->get().transform.translation.x = 5;
    static_cast<void>(scene.build_render_view(cubes, models, lights));
    expect(scene.last_transform_updates() == 2 && cubes[0].model_row_0.w == 5,
           "parent dirtiness reaches child");
    scene.node(child)->get().material.roughness = .4F;
    static_cast<void>(scene.build_render_view(cubes, models, lights));
    expect(scene.last_transform_updates() == 0, "material change does not recompute matrices");
    expect(scene.reparent(child, {}), "reparent succeeds");
    static_cast<void>(scene.build_render_view(cubes, models, lights));
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
    {
        auto source_quality = model;
        source_quality.generate_lods = false;
        const auto vertices = source_quality.vertices;
        vcr::prepare_model_lods(source_quality);
        expect(source_quality.indices == original_indices && source_quality.vertices.size() == vertices.size(),
               "source-quality asset preserves every original triangle");
        const auto& source_mesh = source_quality.submeshes.front();
        expect(source_mesh.lod_count[0] == original_indices.size() && source_mesh.lod_count[1] == 0 &&
                   source_mesh.lod_count[2] == 0 && source_mesh.lod_count[3] == 0,
               "source-quality asset never selects pruned foliage LODs");
    }
    vcr::prepare_model_lods(model);
    const auto &mesh = model.submeshes.front();
    expect(std::equal(original_indices.begin(), original_indices.end(), model.indices.begin()),
           "LOD0 is preserved exactly");
    expect(mesh.lod_count[1] > 0 && mesh.lod_count[1] < mesh.lod_count[0] &&
               mesh.lod_count[2] < mesh.lod_count[1] && mesh.lod_count[3] > 0 && mesh.lod_count[3] < mesh.lod_count[2],
           "LOD simplification reduces triangles");
    for (unsigned level = 1; level < vcr::model_lod_count; ++level) {
        expect(std::isfinite(mesh.lod_error[level]) && mesh.lod_error[level] >= 0,
               "finite geometric error");
        for (unsigned i = 0; i < mesh.lod_count[level]; ++i) {
            expect(model.indices[mesh.lod_first[level] + i] < model.vertices.size(),
                   "LOD index stays in vertex buffer");
        }
    }
    {
        vcr::CookedModelAsset foliage;
        for (unsigned card = 0; card < 64; ++card) {
            const float ox = static_cast<float>(card % 8U) * 0.6F;
            const float oy = static_cast<float>(card / 8U) * 0.6F;
            const float oz = static_cast<float>(card) * 0.02F;
            const auto base_v = static_cast<std::uint32_t>(foliage.vertices.size());
            for (unsigned corner = 0; corner < 4; ++corner) {
                vcr::CookedModelVertex v;
                const float u = static_cast<float>(corner & 1U);
                const float t = static_cast<float>((corner >> 1U) & 1U);
                v.position = {ox + u * 0.35F, oy + t * 0.35F, oz};
                v.normal = {0.0F, 0.0F, 1.0F};
                v.texcoord = {u, t};
                foliage.vertices.push_back(v);
            }
            for (const auto idx : {base_v, base_v + 1U, base_v + 2U, base_v + 2U, base_v + 1U, base_v + 3U}) {
                foliage.indices.push_back(idx);
            }
        }
        const auto foliage_lod0 = foliage.indices;
        foliage.materials.push_back({.coverage = 1U});
        foliage.submeshes.push_back({.index_count = foliage.indices.size(), .material_index = 0U});
        vcr::prepare_model_lods(foliage);
        const auto &cards = foliage.submeshes.front();
        expect(std::equal(foliage_lod0.begin(), foliage_lod0.end(), foliage.indices.begin()),
               "foliage LOD0 preserves disconnected alpha-masked cards bit-exact");
        expect(cards.lod_count[0] >= cards.lod_count[1] && cards.lod_count[1] >= cards.lod_count[2] &&
                   cards.lod_count[2] >= cards.lod_count[3] && cards.lod_count[3] > 0U,
               "all four foliage LOD ranges stay valid even at the minimum card topology");
        for (unsigned level = 1; level < vcr::model_lod_count; ++level) {
            std::array<bool,64> surviving{};
            for (unsigned i = 0; i < cards.lod_count[level]; ++i)
                surviving[foliage.indices[cards.lod_first[level]+i]/4] = true;
            expect(std::ranges::all_of(surviving, [](bool present) { return present; }),
                   "foliage LOD never removes disconnected leaf islands");
            expect(std::isfinite(cards.lod_error[level]) && cards.lod_error[level] > cards.lod_error[level - 1],
                   "foliage geometric errors remain finite and strictly increasing");
        }
    }
    {
        std::array<vcr::CookedModelVertex, 3> tri_vertices{};
        constexpr auto range = 4ULL * 1024 * 1024 * 1024 - 1;
        expect(vcr::rt_masked_triangle_capacity(0, 1, range) == vcr::rt_initial_masked_triangle_capacity,
               "small RT scenes retain the original metadata allocation");
        expect(vcr::rt_masked_triangle_capacity(0, 6'000'001, range) == 8'388'608 &&
                   vcr::rt_masked_triangle_capacity(524'288, 600'000, range) == 1'048'576,
               "resident foliage and streaming RT geometry grow past the former triangle limit");
        const auto limited = 700'000ULL * sizeof(vcr::RtMaskedTriangle) + 17;
        expect(vcr::rt_masked_triangle_capacity(524'288, 650'000, limited) == 700'000 &&
                   vcr::rt_masked_triangle_capacity(524'288, 700'001, limited) == 0 &&
                   vcr::rt_masked_triangle_capacity(0, UINT64_MAX, range) == 0 &&
                   vcr::rt_masked_triangle_capacity(0, 16'000'001, range) == 0,
               "RT metadata obeys whole-record device bounds and the 24-bit custom index budget");
        tri_vertices[0].texcoord = {0.0F, 0.0F};
        tri_vertices[1].texcoord = {1.0F, 0.0F};
        tri_vertices[2].texcoord = {0.0F, 1.0F};
        const std::array<std::uint32_t, 3> tri_indices{0U, 1U, 2U};
        constexpr std::uint32_t masked_half = (32768U << 16U) | 1U;
        expect(vcr::bake_opacity_micromap(tri_vertices, tri_indices, 0U, 3U, nullptr, 1.0F, 0U).empty(),
               "opaque coverage skips software opacity micromap allocation");
        const auto transparent_masks =
            vcr::bake_opacity_micromap(tri_vertices, tri_indices, 0U, 3U, nullptr, 0.1F, masked_half);
        expect(transparent_masks.size() == 1U && transparent_masks[0] == glm::uvec2{0U, 0U},
               "alpha factor below cutoff produces an all-transparent 8x8 micromap");
        const auto fallback_masks =
            vcr::bake_opacity_micromap(tri_vertices, tri_indices, 0U, 3U, nullptr, 1.0F, masked_half);
        expect(fallback_masks.size() == 1U &&
                   fallback_masks[0] == glm::uvec2{0xFFFFFFFFU, 0xFFFFFFFFU},
               "masked material without texture bakes an all-opaque 8x8 micromap");
        vcr::CookedTextureAsset alpha_tex{};
        alpha_tex.width = 1U;
        alpha_tex.height = 4U;
        alpha_tex.row_stride_bytes = 4U;
        // Top rows (v < 0.5 => authoring uv.y > 0.5 after 1-v flip) opaque, bottom rows transparent.
        alpha_tex.pixels = {
            255U, 255U, 255U, 255U,
            255U, 255U, 255U, 255U,
            255U, 255U, 255U, 0U,
            255U, 255U, 255U, 0U,
        };
        const auto split_masks = vcr::bake_opacity_micromap(
            tri_vertices, tri_indices, 0U, 3U, &alpha_tex, 1.0F, masked_half);
        // At (ix=0, iy=0): s=0.0625, t=0.0625 => uv.y = b2 = s*t ~= 0.0039 => v ~= 0.996 => alpha=0 (bit 0 clear).
        // At (ix=7, iy=7): s=0.9375, t=0.9375 => uv.y = b2 = s*t ~= 0.8789 => v ~= 0.121 => alpha=1 (bit 63 set).
        expect(split_masks.size() == 1U &&
                   (split_masks[0].x & 1U) == 0U &&
                   (split_masks[0].y & 0x80000000U) != 0U,
               "8x8 software opacity micromap respects authoring V-flip and alpha cutoff");

        const auto packed_alpha = vcr::pack_alpha_r8_1024_words(alpha_tex);
        expect(packed_alpha.size() == vcr::rt_alpha_map_word_count &&
                   (packed_alpha[256U * 256U] & 0xFFU) == 255U &&
                   (packed_alpha[768U * 256U] & 0xFFU) == 0U,
               "1024x1024 R8 alpha table preserves top-to-bottom alpha polarity");
        const auto masked_tris =
            vcr::bake_rt_masked_triangles(tri_vertices, tri_indices, 0U, 3U, true, 1.0F, masked_half);
        expect(masked_tris.size() == 1U &&
                   std::abs(masked_tris[0].uv0[1] - 1.0F) < 1e-6F &&
                   std::abs(masked_tris[0].uv2[1] - 0.0F) < 1e-6F &&
                   masked_tris[0].effective_cutoff > 0.49F &&
                   masked_tris[0].effective_cutoff < 0.51F,
               "RT masked triangle baker V-flips UVs and computes effective alpha cutoff");
    }
    {
        // A missing image proves the second model does not even reopen/decode
        // an opaque texture that is already resident. Alpha pixels remain
        // necessary for opacity micromaps, including on cache hits.
        const auto root = std::filesystem::temp_directory_path() /
            ("vcr-resident-texture-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        expect(std::filesystem::create_directory(root), "create owned resident-texture fixture");
        std::filesystem::create_directory(root / "materials");
        std::filesystem::create_directory(root / "textures");
        const auto write_text = [](const auto& path, const char* text) {
            std::ofstream stream(path, std::ios::binary);
            stream << text;
        };
        write_text(root / "triangle.vcrasset.json", R"({"schema":"vcr.asset","version":1,"kind":"model","name":"triangle","mesh":"triangle.vcrmesh","geometry":{"vertex_count":3,"index_count":3,"index_type":"uint32","primitive":"triangles","vertex_stride":48},"materials":["materials/shared.vcrmaterial.json"],"submeshes":[{"name":"triangle","first_index":0,"index_count":3,"material_index":0,"material":"materials/shared.vcrmaterial.json"}]})");
        const auto write_material = [&](bool masked) {
            std::ofstream stream(root / "materials/shared.vcrmaterial.json");
            stream << R"({"schema":"vcr.asset","version":1,"kind":"material","name":"shared","pbr":{"base_color":[1,1,1,1],"metallic":0,"roughness":0.7,"ambient_occlusion":1},"textures":{"base_color":"textures/shared.vcrtexture.json"},"alpha_mode":")"
                   << (masked ? "mask" : "opaque") << R"("})";
        };
        write_material(false);
        write_text(root / "textures/shared.vcrtexture.json", R"({"schema":"vcr.asset","version":1,"kind":"texture","name":"shared","source":"textures/missing.png","usage":"base_color","color_space":"srgb"})");
        {
            std::ofstream stream(root / "triangle.vcrmesh", std::ios::binary);
            stream.write("VCRMESH\0", 8);
            const auto u32 = [&](std::uint32_t value) { stream.write(reinterpret_cast<const char*>(&value),4); };
            const auto u64 = [&](std::uint64_t value) { stream.write(reinterpret_cast<const char*>(&value),8); };
            u32(1); u32(48); u64(3); u64(3); u32(1); u32(0);
            u64(0); u64(3); u32(0); u32(0);
            std::array<vcr::CookedModelVertex,3> vertices{};
            vertices[0].position = {-1,0,0}; vertices[1].position = {1,0,0}; vertices[2].position = {0,1,0};
            for (auto& vertex : vertices) { vertex.normal = {0,0,1}; vertex.tangent = {1,0,0,1}; }
            stream.write(reinterpret_cast<const char*>(vertices.data()),sizeof(vertices));
            u32(0); u32(1); u32(2);
        }
        const std::vector<std::string> resident{"textures/shared.vcrtexture.json"};
        const auto first = vcr::prepare_model_asset(root, "triangle.vcrasset.json");
        expect(first.model && first.textures.size() == 1 && !first.textures[0].error.empty(),
               "nonresident texture still attempts decode and reports missing image");
        const auto reused = vcr::prepare_model_asset(root, "triangle.vcrasset.json", resident);
        expect(reused.model && reused.textures.empty(), "resident opaque texture is not decoded twice");
        write_material(true);
        const auto alpha = vcr::prepare_model_asset(root, "triangle.vcrasset.json", resident);
        expect(alpha.model && alpha.textures.size() == 1 && !alpha.textures[0].error.empty(),
               "masked resident texture still obtains alpha pixels for OMM");
        write_material(false);
        vcr::AssetPreparation resident_worker;
        resident_worker.start();
        auto owned_snapshot = resident;
        expect(resident_worker.request(root, "triangle.vcrasset.json", owned_snapshot), "queue resident-texture snapshot");
        owned_snapshot.clear();
        std::optional<vcr::PreparedModel> result;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result && std::chrono::steady_clock::now() < until) {
            result = resident_worker.poll();
            if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(result && result->model && result->textures.empty(), "worker owns resident snapshot after caller clears it");
        resident_worker.stop();
        std::filesystem::remove_all(root);
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
