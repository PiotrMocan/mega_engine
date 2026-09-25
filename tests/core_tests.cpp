#include "asset_browser.hpp"
#include "editor_scene_tools.hpp"
#include "cli_options.hpp"
#include "coarse_depth_order.hpp"
#include "fly_camera.hpp"
#include "mcp_control.hpp"
#include "performance_metrics.hpp"
#include "png_encoder.hpp"
#include "render_fixture_scene.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "temporal_coordinates.hpp"
#include "camera_projection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool close_to(const double value, const double expected, const double epsilon) {
    return std::abs(value - expected) <= epsilon;
}

} // namespace

int main() {
    {
        const char* args[]{"vcr", "--fog=height", "--fog-density", "0.01",
            "--fog-base-height=-20", "--fog-height-falloff=0.1", "--fog-start=25",
            "--fog-distance=500", "--fog-max-opacity=0.7", "--fog-color-r=2",
            "--fog-color-g=0.3", "--fog-color-b=0.4", "--aa=msaa4x"};
        const auto parsed = vcr::parse_cli_options(static_cast<int>(std::size(args)), args);
        expect(parsed && parsed.options.fog.enabled &&
                   parsed.options.fog.density == 0.01F &&
                   parsed.options.fog.base_height == -20.0F &&
                   parsed.options.fog.height_falloff == 0.1F &&
                   parsed.options.fog.start_distance == 25.0F &&
                   parsed.options.fog.maximum_distance == 500.0F &&
                   parsed.options.fog.maximum_opacity == 0.7F &&
                   parsed.options.fog.color == std::array{2.0F, 0.3F, 0.4F},
               "height fog accepts explicit HDR, distance and height parameters with MSAA");
        for (const char* invalid : {"--fog=on", "--fog=", "--fog-density=-1",
                                   "--fog-density=nan", "--fog-density=1.01",
                                   "--fog-height-falloff=-1", "--fog-height-falloff=11",
                                   "--fog-base-height=100001", "--fog-distance=0",
                                   "--fog-start=1000", "--fog-max-opacity=1.1",
                                   "--fog-color-r=-1", "--fog-color-g=inf", "--fog-color-b=17"}) {
            const char* bad[]{"vcr", invalid};
            expect(!vcr::parse_cli_options(2, bad), "invalid fog options are rejected");
        }
        const char* missing[]{"vcr", "--fog-density"};
        const char* reset[]{"vcr", "--fog=height", "--fog=off"};
        const auto disabled = vcr::parse_cli_options(3, reset);
        expect(!vcr::parse_cli_options(2, missing) && disabled && !disabled.options.fog.enabled,
               "fog requires option values and can be explicitly disabled");
        const char* partial_args[]{"vcr", "--fog-density", "0.25"};
        const auto partial = vcr::parse_cli_options(3, partial_args);
        vcr::FogSettings authored_fog{
            .enabled = true,
            .color = {1.0F, 2.0F, 3.0F},
            .density = 0.01F,
            .height_falloff = 0.2F,
            .base_height = -5.0F,
            .start_distance = 25.0F,
            .maximum_distance = 500.0F,
            .maximum_opacity = 0.7F,
        };
        const auto merged_fog = vcr::resolve_scene_fog(partial.options, authored_fog);
        expect(partial && merged_fog.enabled && merged_fog.density == 0.25F &&
                   merged_fog.color == authored_fog.color &&
                   merged_fog.height_falloff == authored_fog.height_falloff &&
                   merged_fog.maximum_distance == authored_fog.maximum_distance,
               "CLI fog overrides only explicit authored fields");
        expect(vcr::cli_usage("vcr").find("--fog off|height") != std::string::npos,
               "CLI help exposes atmospheric fog");
    }
    {
        for (const float ior : {1.0F, 1.33F, 1.5F, 2.0F, 2.42F, 10.0F, 1.0e20F,
                                std::numeric_limits<float>::max()}) {
            const double ratio = (static_cast<double>(ior) - 1.0) / (static_cast<double>(ior) + 1.0);
            expect(close_to(vcr::dielectric_f0(ior), ratio * ratio, 2.0e-7),
                   "dielectric F0 matches the independent double Fresnel reference");
        }
        expect(vcr::dielectric_f0(1.5F) == 0.04F && vcr::dielectric_f0(1.0F) == 0.0F,
               "default and index-matched F0 preserve exact endpoint values");
        vcr::PbrMaterial material;
        material.ior = std::numeric_limits<float>::infinity();
        material.model = static_cast<vcr::MaterialModel>(99);
        material.subsurface_profile = static_cast<vcr::SubsurfaceProfile>(99);
        material.subsurface_strength = std::numeric_limits<float>::infinity();
        material = vcr::sanitize_pbr_material(material);
        expect(material.ior == 1.5F && material.model == vcr::MaterialModel::standard &&
                   material.subsurface_profile == vcr::SubsurfaceProfile::none &&
                   material.subsurface_strength == 0.0F,
               "invalid material optics sanitize to the established defaults and disable SSS");
        expect(vcr::parse_material_model("standard") == vcr::MaterialModel::standard &&
                   vcr::parse_material_model("unlit") == vcr::MaterialModel::unlit &&
                   vcr::parse_material_model("subsurface") == vcr::MaterialModel::subsurface &&
                   !vcr::parse_material_model("Unlit") && !vcr::parse_material_model("1"),
               "material model tokens are explicit and unknown models are not silently enabled");
        const char* fixture[]{"vcr", "--render-fixture=material-spheres"};
        const auto parsed = vcr::parse_cli_options(2, fixture);
        expect(parsed && parsed.options.render_fixture == vcr::RenderFixture::material_spheres,
               "material sphere fixture is exposed through the strict renderer CLI");
    }
    {
        expect(!vcr::CliOptions{}.cpu_hierarchy, "linear CPU culling remains the default");
        const char* hierarchy[]{"vcr", "--cpu-culling=hierarchy"};
        const char* linear[]{"vcr", "--cpu-culling", "linear"};
        const auto selected = vcr::parse_cli_options(2, hierarchy);
        const auto baseline = vcr::parse_cli_options(3, linear);
        expect(selected && selected.options.cpu_hierarchy && baseline && !baseline.options.cpu_hierarchy,
               "CPU culling selects an explicit hierarchy or linear reference");
        for (const char* token : {"", "auto", "Hierarchy", "hierarchy ", "--aa=off"}) {
            const char* args[]{"vcr", "--cpu-culling", token};
            expect(!vcr::parse_cli_options(3, args), "CPU culling rejects invalid mode tokens");
        }
        const char* missing[]{"vcr", "--cpu-culling"};
        expect(!vcr::parse_cli_options(2, missing), "CPU culling requires an argument");
        vcr::BenchmarkResult result;
        result.cpu_culling_requested = "hierarchy";
        const auto json = nlohmann::json::parse(vcr::benchmark_result_to_json(result));
        expect(json.at("cpu_culling_requested") == "hierarchy" && json.at("cpu_culling_actual") == "off",
               "benchmark records hierarchy request separately from the active culling path");
    }
    {
        expect(!vcr::CliOptions{}.coarse_front_to_back,
               "coarse front-to-back sorting is disabled by default");
        for (const char* mode : {"off", "coarse"}) {
            const char* args[]{"vcr", "--front-to-back", mode};
            const auto parsed = vcr::parse_cli_options(3, args);
            const auto inline_argument = std::string{"--front-to-back="} + mode;
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            const auto inline_parsed = vcr::parse_cli_options(2, inline_args);
            const bool enabled = std::string_view(mode) == "coarse";
            expect(parsed && inline_parsed && parsed.options.coarse_front_to_back == enabled &&
                       inline_parsed.options.coarse_front_to_back == enabled,
                   "front-to-back mode accepts both strict CLI spellings");
        }
        for (const char* bad : {"", "on", "auto", "Coarse", "coarse ", " coarse", "1"}) {
            const char* args[]{"vcr", "--front-to-back", bad};
            const auto inline_argument = std::string{"--front-to-back="} + bad;
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            expect(!vcr::parse_cli_options(3, args) && !vcr::parse_cli_options(2, inline_args),
                   "front-to-back mode rejects unknown tokens and whitespace");
        }
        const char* missing[]{"vcr", "--front-to-back"};
        const char* next_option[]{"vcr", "--front-to-back", "--aa=off"};
        const char* reset[]{"vcr", "--front-to-back=coarse", "--front-to-back=off"};
        const auto disabled = vcr::parse_cli_options(3, reset);
        expect(!vcr::parse_cli_options(2, missing) && !vcr::parse_cli_options(3, next_option) &&
                   disabled && !disabled.options.coarse_front_to_back,
               "front-to-back requires a value and explicit off resets coarse mode");
        expect(vcr::cli_usage("vcr").find("--front-to-back") != std::string::npos,
               "CLI help documents optional CPU front-to-back sorting");
        vcr::BenchmarkResult result;
        expect(result.front_to_back_requested == "off" && result.front_to_back_actual == "off",
               "front-to-back benchmark metadata defaults to off");
        result.front_to_back_requested = "coarse";
        const auto json = nlohmann::json::parse(vcr::benchmark_result_to_json(result));
        expect(json.at("front_to_back_requested") == "coarse" &&
                   json.at("front_to_back_actual") == "off",
               "benchmark metadata distinguishes requested sorting from an unavailable path");
    }
    {
        const auto vp = vcr::camera_projection(16.0F / 9.0F);
        const vcr::Bounds exact{{0, 0, -17}, {1, 1, 1}};
        const vcr::Bounds preceding{{0, 0, -16.99F}, {1, 1, 1}};
        expect(vcr::coarse_depth_key(exact, vp) == 2 &&
                   vcr::coarse_depth_key(preceding, vp) == 1,
               "coarse depth uses nearest AABB distance and eight metre boundaries");
        expect(vcr::coarse_depth_key({{0, 0, -.5F}, {1, 1, 1}}, vp) == 0 &&
                   vcr::coarse_depth_key({{0, 0, 20}, {1, 1, 1}}, vp) == 0,
               "eye-plane-crossing and behind-camera bounds clamp to the first bucket");
        auto oblique = glm::mat4(1.0F);
        oblique[0][3] = 2; oblique[1][3] = -3;
        oblique[2][3] = -4; oblique[3][3] = 8;
        expect(vcr::coarse_depth_key({{10, 10, -20}, {1, 2, 3}}, oblique) == 7,
               "coarse depth projects the AABB support radius through the complete clip-w row");
        auto jittered = vp;
        jittered[2][0] += .001F; jittered[2][1] -= .002F;
        const auto ordinary = glm::perspectiveRH_ZO(glm::radians(65.0F), 16.0F / 9.0F, .1F, 500.0F);
        expect(vcr::coarse_depth_key(exact, jittered) == vcr::coarse_depth_key(exact, vp) &&
                   vcr::coarse_depth_key(exact, ordinary) == vcr::coarse_depth_key(exact, vp),
               "coarse ordering is independent of projection jitter and reverse-Z encoding");
        constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
        const float huge = std::numeric_limits<float>::max();
        expect(vcr::coarse_depth_key({{0, 0, -huge}, {0, 0, 0}}, vp) == maximum,
               "huge finite distances saturate before unsigned conversion");
        auto huge_projection = vp;
        huge_projection[2][3] = -huge;
        expect(vcr::coarse_depth_key({{0, 0, -huge}, {0, 0, 0}}, huge_projection) == maximum,
               "finite float products do not overflow intermediate depth arithmetic");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        auto invalid_projection = vp;
        invalid_projection[3][3] = nan;
        expect(vcr::coarse_depth_key({{nan, 0, -17}, {1, 1, 1}}, vp) == maximum &&
                   vcr::coarse_depth_key({{0, 0, -17}, {infinity, 1, 1}}, vp) == maximum &&
                   vcr::coarse_depth_key({{0, 0, -17}, {-1, 1, 1}}, vp) == maximum &&
                   vcr::coarse_depth_key(exact, invalid_projection) == maximum,
               "nonfinite inputs and invalid extents sort last without undefined conversion");
        const vcr::Bounds same_bucket{{0, 0, -20}, {1, 1, 1}};
        const auto earlier_source = std::pair{vcr::coarse_depth_key(same_bucket, vp), 3U};
        const auto later_source = std::pair{vcr::coarse_depth_key(exact, vp), 8U};
        expect(earlier_source < later_source && !(later_source < earlier_source),
               "equal depth buckets retain deterministic source-index order");
    }
    {
        using Pattern = vcr::LodFadePattern;
        expect(vcr::CliOptions{}.lod_fade == Pattern::off &&
                   vcr::CliOptions{}.lod_fade_seconds == 0.25F,
               "model LOD fade defaults to off and a quarter second");
        const std::array patterns{Pattern::off, Pattern::bayer4, Pattern::bayer8};
        const std::array<const char*, 3> tokens{"off", "bayer4", "bayer8"};
        for (std::size_t i = 0; i < patterns.size(); ++i) {
            const char* args[]{"vcr", "--lod-fade", tokens[i]};
            const auto parsed = vcr::parse_cli_options(3, args);
            const auto inline_argument = std::string{"--lod-fade="} + tokens[i];
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            const auto inline_parsed = vcr::parse_cli_options(2, inline_args);
            expect(parsed && inline_parsed && parsed.options.lod_fade == patterns[i] &&
                       inline_parsed.options.lod_fade == patterns[i] &&
                       vcr::to_token(patterns[i]) == tokens[i] &&
                       vcr::parse_lod_fade_pattern(tokens[i]) == patterns[i],
                   "model LOD fade tokens and both CLI spellings parse");
        }
        for (const char* bad : {"", "on", "auto", "blue-noise", "Bayer4", "BAYER8",
                                "bayer4 ", " bayer8", "1"}) {
            const char* args[]{"vcr", "--lod-fade", bad};
            const auto inline_argument = std::string{"--lod-fade="} + bad;
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            expect(!vcr::parse_cli_options(3, args) && !vcr::parse_cli_options(2, inline_args) &&
                       !vcr::parse_lod_fade_pattern(bad),
                   "model LOD fade rejects unsupported tokens without normalization");
        }
        const std::array<const char*, 4> durations{"0.05", ".25", "0.75", "2"};
        const std::array values{0.05F, 0.25F, 0.75F, 2.0F};
        for (std::size_t i = 0; i < durations.size(); ++i) {
            const char* args[]{"vcr", "--lod-fade-seconds", durations[i]};
            const auto parsed = vcr::parse_cli_options(3, args);
            const auto inline_argument = std::string{"--lod-fade-seconds="} + durations[i];
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            const auto inline_parsed = vcr::parse_cli_options(2, inline_args);
            expect(parsed && inline_parsed && parsed.options.lod_fade_seconds == values[i] &&
                       inline_parsed.options.lod_fade_seconds == values[i] &&
                       parsed.options.lod_fade == Pattern::off,
                   "LOD fade duration accepts inclusive bounds and preserves the selected pattern");
        }
        for (const char* bad : {"", "nan", "NaN", "inf", "-inf", "1e100", "0", "-1",
                                "0.0499999999", "2.0000000001", "true", "null", "[1]",
                                "x", "0.5x", " 0.5", "0.5 ", "+0.5"}) {
            const char* args[]{"vcr", "--lod-fade-seconds", bad};
            const auto inline_argument = std::string{"--lod-fade-seconds="} + bad;
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            expect(!vcr::parse_cli_options(3, args) && !vcr::parse_cli_options(2, inline_args),
                   "LOD fade duration rejects invalid or out-of-range doubles before float conversion");
        }
        for (const char* option : {"--lod-fade", "--lod-fade-seconds"}) {
            const char* missing[]{"vcr", option};
            const char* next_option[]{"vcr", option, "--aa=off"};
            expect(!vcr::parse_cli_options(2, missing) && !vcr::parse_cli_options(3, next_option),
                   "model LOD fade rejects missing arguments");
        }
        const char* reset[]{"vcr", "--lod-fade=bayer8", "--lod-fade-seconds=0.75", "--lod-fade=off"};
        const auto disabled = vcr::parse_cli_options(4, reset);
        expect(disabled && disabled.options.lod_fade == Pattern::off &&
                   disabled.options.lod_fade_seconds == 0.75F,
               "explicit fade off preserves the configured duration");
        expect(vcr::to_token(static_cast<Pattern>(255)) == "unknown",
               "unknown model LOD fade enum has no valid token");
        const auto usage = vcr::cli_usage("vcr");
        expect(usage.find("--lod-fade PATTERN") != std::string::npos &&
                   usage.find("--lod-fade-seconds SEC") != std::string::npos,
               "CLI help documents both model LOD fade controls");
        vcr::BenchmarkResult result;
        expect(result.lod_fade_requested == "off" && result.lod_fade_actual == "off" &&
                   result.lod_fade_seconds_requested == 0.25F &&
                   result.lod_fade_seconds_actual == 0.25F,
               "benchmark model LOD fade defaults are explicit");
        result.lod_fade_requested = "bayer8";
        result.lod_fade_seconds_requested = 0.75F;
        result.lod_fade_seconds_actual = 1.5F;
        const auto json = nlohmann::json::parse(vcr::benchmark_result_to_json(result));
        expect(json.at("lod_fade_requested") == "bayer8" && json.at("lod_fade_actual") == "off" &&
                   json.at("lod_fade_seconds_requested") == 0.75 &&
                   json.at("lod_fade_seconds_actual") == 1.5,
               "benchmark keeps requested and actual LOD fade policy and duration independent");
    }
    {
        using Prepass = vcr::DepthPrepass;
        expect(vcr::CliOptions{}.depth_prepass == Prepass::off,
               "depth prepass is disabled by default");
        const std::array modes{Prepass::off, Prepass::partial, Prepass::full};
        const std::array<const char*, 3> tokens{"off", "partial", "full"};
        for (std::size_t i = 0; i < modes.size(); ++i) {
            const char* args[]{"vcr", "--depth-prepass", tokens[i]};
            const auto parsed = vcr::parse_cli_options(3, args);
            const auto inline_argument = std::string{"--depth-prepass="} + tokens[i];
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            const auto inline_parsed = vcr::parse_cli_options(2, inline_args);
            expect(parsed && parsed.options.depth_prepass == modes[i] &&
                       inline_parsed && inline_parsed.options.depth_prepass == modes[i] &&
                       vcr::to_token(modes[i]) == tokens[i] &&
                       vcr::parse_depth_prepass(tokens[i]) == modes[i],
                   "depth prepass tokens and both CLI spellings parse");
        }
        for (const char* bad : {"", "on", "auto", "Partial", "FULL", "partial ", " full", "1"}) {
            const char* args[]{"vcr", "--depth-prepass", bad};
            const auto inline_argument = std::string{"--depth-prepass="} + bad;
            const char* inline_args[]{"vcr", inline_argument.c_str()};
            expect(!vcr::parse_cli_options(3, args) &&
                       !vcr::parse_cli_options(2, inline_args) && !vcr::parse_depth_prepass(bad),
                   "unknown depth prepass tokens are rejected strictly");
        }
        const char* missing[]{"vcr", "--depth-prepass"};
        const char* next_option[]{"vcr", "--depth-prepass", "--aa=off"};
        expect(!vcr::parse_cli_options(2, missing) && !vcr::parse_cli_options(3, next_option),
               "missing depth prepass mode is rejected");
        const char* reset[]{"vcr", "--depth-prepass=full", "--depth-prepass=off"};
        const auto disabled = vcr::parse_cli_options(3, reset);
        expect(disabled && disabled.options.depth_prepass == Prepass::off,
               "explicit off resets a previously selected depth prepass");
        expect(vcr::to_token(static_cast<Prepass>(255)) == "unknown",
               "unknown depth prepass enum has no valid token");
        expect(vcr::cli_usage("vcr").find("--depth-prepass") != std::string::npos,
               "CLI help documents depth prepass selection");
    }
    {
        using Fixture = vcr::RenderFixture;
        expect(vcr::CliOptions{}.render_fixture == Fixture::none,
               "procedural render fixture is disabled by default");
        expect(vcr::to_token(Fixture::none) == "none" &&
                   vcr::to_token(Fixture::cascade_cache) == "cascade-cache" &&
                   vcr::to_token(Fixture::subsurface_profiles) == "subsurface-profiles" &&
                   vcr::to_token(Fixture::light_swarm) == "light-swarm" &&
                   vcr::to_token(Fixture::aa_wires) == "aa-wires" &&
                   vcr::to_token(Fixture::motion_bars) == "motion-bars" &&
                   vcr::parse_render_fixture("none") == Fixture::none &&
                   vcr::parse_render_fixture("cascade-cache") == Fixture::cascade_cache &&
                   vcr::parse_render_fixture("subsurface-profiles") == Fixture::subsurface_profiles &&
                   vcr::parse_render_fixture("light-swarm") == Fixture::light_swarm &&
                   vcr::parse_render_fixture("aa-wires") == Fixture::aa_wires &&
                   vcr::parse_render_fixture("motion-bars") == Fixture::motion_bars,
                "render fixture tokens are stable");
        for (const auto fixture : {Fixture::overdraw_1, Fixture::overdraw_4, Fixture::overdraw_16}) {
            const auto token = vcr::to_token(fixture);
            const char* args[]{"vcr", "--render-fixture", token.data()};
            const auto option = vcr::parse_cli_options(3, args);
            expect(option && option.options.render_fixture == fixture &&
                       vcr::parse_render_fixture(token) == fixture,
                   "controlled overdraw fixture selects an exact layer count");
        }
        const char* selected[]{"vcr", "--render-fixture", "cascade-cache"};
        const auto parsed = vcr::parse_cli_options(3, selected);
        expect(parsed && parsed.options.render_fixture == Fixture::cascade_cache,
               "cascade fixture CLI selection parses");
        const char* disabled[]{"vcr", "--render-fixture=cascade-cache", "--render-fixture=none"};
        const auto reset = vcr::parse_cli_options(3, disabled);
        expect(reset && reset.options.render_fixture == Fixture::none,
               "explicit none resets a previously selected fixture");
        const char* missing[]{"vcr", "--render-fixture"};
        expect(!vcr::parse_cli_options(2, missing), "missing render fixture is rejected");
        for (const char* bad : {"", "cascade_cache", "Cascade-cache", "cascade-cache ",
                                "aa_wires", "AA-wires", "aa-wires ", "auto"}) {
            const char* args[]{"vcr", "--render-fixture", bad};
            expect(!vcr::parse_cli_options(3, args) && !vcr::parse_render_fixture(bad),
                   "unknown render fixture tokens are rejected strictly");
        }
        for (const char* selector : {"--lighting-room", "--scene-count=128"}) {
            const char* first[]{"vcr", "--render-fixture=cascade-cache", selector};
            const char* last[]{"vcr", selector, "--render-fixture=cascade-cache"};
            const auto fixture_first = vcr::parse_cli_options(3, first);
            const auto fixture_last = vcr::parse_cli_options(3, last);
            expect(!fixture_first && !fixture_last &&
                       fixture_first.error.find("cannot be combined") != std::string::npos &&
                       fixture_last.error.find("cannot be combined") != std::string::npos,
                   "conflicting fixture scene selectors are rejected in both flag orders");
            const char* none[]{"vcr", selector, "--render-fixture=none"};
            expect(static_cast<bool>(vcr::parse_cli_options(3, none)),
                   "explicit none permits another scene selector");
        }
        const char* compatible[]{"vcr", "--render-fixture=cascade-cache", "--camera-z=12",
                                 "--lighting=dynamic", "--animate-scene"};
        const auto configured = vcr::parse_cli_options(5, compatible);
        expect(configured && configured.options.render_fixture == Fixture::cascade_cache &&
                   configured.options.camera_z_explicit && configured.options.lighting.dynamic &&
                   configured.options.animate_scene,
                "render fixture permits camera lighting and caster animation options");
        const char* swarm[]{"vcr", "--render-fixture=light-swarm", "--local-light-count=4096",
                            "--light-swarm-layout=overlap"};
        const auto swarm_options = vcr::parse_cli_options(4, swarm);
        expect(swarm_options && swarm_options.options.render_fixture == Fixture::light_swarm &&
                   swarm_options.options.local_light_count == vcr::maximum_local_lights &&
                   swarm_options.options.light_swarm_layout == vcr::LightSwarmLayout::overlap,
               "light swarm accepts bounded count and layout controls");
        for (const char* bad_count : {"4097", "-1", "1.5"}) {
            const char* args[]{"vcr", "--render-fixture=light-swarm", "--local-light-count", bad_count};
            expect(!vcr::parse_cli_options(4, args), "light swarm rejects invalid counts");
        }
        const char* unrelated[]{"vcr", "--local-light-count=16"};
        expect(!vcr::parse_cli_options(2, unrelated),
               "light swarm controls cannot be silently ignored by another scene");
        auto swarm_scene = vcr::make_light_swarm_fixture(
            12.0F, 256U, vcr::LightSwarmLayout::overlap);
        std::vector<vcr::SceneGpuInstance> swarm_cubes;
        std::vector<vcr::SceneModelInstance> swarm_models;
        std::vector<vcr::ScenePunctualLight> swarm_lights;
        const auto swarm_view = swarm_scene.build_render_view(
            swarm_cubes, swarm_models, swarm_lights);
        expect(swarm_view.local_lights.size() == 256U && !swarm_view.local_lights_overflow &&
                   swarm_cubes.size() == 4U && swarm_view.sun_visible && swarm_view.sky_visible,
               "light swarm uses the regular bounded scene/render-view path");

        const char* aa_args[]{"vcr", "--render-fixture=aa-wires"};
        const auto aa_options = vcr::parse_cli_options(2, aa_args);
        expect(aa_options && aa_options.options.render_fixture == Fixture::aa_wires,
               "AA wire fixture is exposed through the strict renderer CLI");
        expect(vcr::cli_usage("vcr").find("aa-wires") != std::string::npos,
               "CLI help documents the AA wire quality fixture");
        auto aa_scene = vcr::make_aa_wires_fixture(12.0F);
        std::vector<vcr::SceneGpuInstance> aa_cubes;
        std::vector<vcr::SceneModelInstance> aa_models;
        std::vector<vcr::ScenePunctualLight> aa_lights;
        const auto aa_view = aa_scene.build_render_view(aa_cubes, aa_models, aa_lights);
        const bool all_unlit = std::ranges::all_of(
            aa_cubes, [](const vcr::SceneGpuInstance& cube) {
                return cube.material.model == vcr::MaterialModel::unlit;
            });
        std::vector<vcr::SceneNodeRecord> aa_records;
        aa_scene.build_node_records(aa_records);
        const auto named = [&](const std::string_view name) {
            return std::ranges::any_of(aa_records, [&](const vcr::SceneNodeRecord& record) {
                return record.value.name == name;
            });
        };
        expect(aa_view.cubes.size() == 17U && aa_scene.size() == 17U &&
                   aa_view.models.empty() && aa_view.local_lights.empty() &&
                   !aa_view.sun_visible && !aa_view.sky_visible && all_unlit &&
                   named("AA dark background") && named("AA red boundary") &&
                   named("AA green boundary") && named("AA far blue panel") &&
                   named("AA near cross horizontal") && named("AA near cross vertical") &&
                   named("AA black interior control") && named("AA grey interior control") &&
                   named("AA white interior control"),
               "AA wire fixture is deterministic, unlit, and contains edge and interior controls");

        const char* motion_args[]{"vcr", "--render-fixture=motion-bars",
                                   "--animate-scene", "--motion-blur=all",
                                   "--motion-fixture-fps", "60"};
        const auto motion_options = vcr::parse_cli_options(
            static_cast<int>(std::size(motion_args)), motion_args);
        expect(motion_options &&
                   motion_options.options.render_fixture == Fixture::motion_bars &&
                   motion_options.options.animate_scene &&
                   motion_options.options.motion_fixture_fps == 60U &&
                   motion_options.options.motion_blur.mode == vcr::MotionBlurMode::all,
               "motion-bar fixture parses a deterministic simulation rate");
        const char* motion_30[]{"vcr", "--render-fixture", "motion-bars",
                                "--animate-scene", "--motion-fixture-fps=30"};
        const char* motion_144[]{"vcr", "--render-fixture", "motion-bars",
                                 "--animate-scene", "--motion-fixture-fps=144"};
        const char* motion_bad_rate[]{"vcr", "--render-fixture", "motion-bars",
                                      "--animate-scene", "--motion-fixture-fps", "120"};
        const char* motion_wrong_fixture[]{"vcr", "--render-fixture", "aa-wires",
                                           "--animate-scene", "--motion-fixture-fps", "60"};
        const char* motion_not_animated[]{"vcr", "--render-fixture", "motion-bars",
                                          "--motion-fixture-fps", "60"};
        expect(vcr::parse_cli_options(static_cast<int>(std::size(motion_30)), motion_30) &&
                   vcr::parse_cli_options(static_cast<int>(std::size(motion_144)), motion_144) &&
                   !vcr::parse_cli_options(
                       static_cast<int>(std::size(motion_bad_rate)), motion_bad_rate) &&
                   !vcr::parse_cli_options(
                       static_cast<int>(std::size(motion_wrong_fixture)), motion_wrong_fixture) &&
                   !vcr::parse_cli_options(
                       static_cast<int>(std::size(motion_not_animated)), motion_not_animated) &&
                   vcr::CliOptions{}.motion_fixture_fps == 0U &&
                   vcr::cli_usage("vcr").find("--motion-fixture-fps") != std::string::npos,
               "fixed motion fixture rate is bounded and fixture-local");
        auto motion_scene = vcr::make_motion_bars_fixture(12.0F);
        std::vector<vcr::SceneGpuInstance> motion_cubes;
        std::vector<vcr::SceneModelInstance> motion_models;
        std::vector<vcr::ScenePunctualLight> motion_lights;
        const auto motion_view = motion_scene.build_render_view(
            motion_cubes, motion_models, motion_lights);
        const bool motion_unlit = std::ranges::all_of(
            motion_cubes, [](const vcr::SceneGpuInstance& cube) {
                return cube.material.model == vcr::MaterialModel::unlit;
            });
        expect(motion_view.cubes.size() == 6U &&
                   motion_scene.size() == 6U && motion_unlit &&
                   motion_view.models.empty() && motion_view.local_lights.empty() &&
                   !motion_view.sun_visible && !motion_view.sky_visible,
               "motion fixture has counter-moving bars, depth markers, and no lighting noise");
    }
    {
        using Mode = vcr::SubsurfaceMode;
        using Profile = vcr::SubsurfaceProfile;
        const char* enabled[]{"vcr", "--subsurface=full",
                              "--render-fixture", "subsurface-profiles"};
        const auto parsed = vcr::parse_cli_options(4, enabled);
        expect(parsed && parsed.options.subsurface == Mode::full_resolution &&
                   parsed.options.render_fixture == vcr::RenderFixture::subsurface_profiles,
               "SSS mode and procedural profile fixture parse together");
        const char* invalid[]{"vcr", "--subsurface", "half"};
        expect(!vcr::parse_cli_options(3, invalid) &&
                   vcr::CliOptions{}.subsurface == Mode::off &&
                   vcr::cli_usage("vcr").find("--subsurface") != std::string::npos,
               "SSS stays default-off and rejects unsupported modes");
        for (const auto profile : {
                 Profile::narrow, Profile::medium, Profile::wide}) {
            for (const float strength : {0.0F, 0.5F, 1.0F}) {
                const float packed = vcr::pack_subsurface_control(profile, strength);
                expect(vcr::unpack_subsurface_profile(packed) == profile &&
                           close_to(vcr::unpack_subsurface_strength(packed),
                                    std::round(strength * 255.0F) / 255.0F, 0.000001),
                       "FP16-safe SSS control round-trips profile and quantized strength");
            }
        }
        const auto narrow = vcr::subsurface_profile_parameters(Profile::narrow);
        const auto medium = vcr::subsurface_profile_parameters(Profile::medium);
        const auto wide = vcr::subsurface_profile_parameters(Profile::wide);
        expect(narrow.scatter_distance[0] < medium.scatter_distance[0] &&
                   medium.scatter_distance[0] < wide.scatter_distance[0] &&
                   wide.scatter_distance[0] > wide.scatter_distance[1] &&
                   wide.scatter_distance[1] > wide.scatter_distance[2],
               "reference SSS profiles widen monotonically and scatter red furthest");
    }
    {
        using Mode = vcr::MotionBlurMode;
        const char* enabled[]{
            "vcr", "--motion-blur", "all",
            "--motion-blur-quality=low",
            "--motion-blur-shutter", "trailing",
            "--motion-blur-angle=270",
            "--motion-blur-radius", "24",
            "--motion-blur-camera-fade-start=25",
            "--motion-blur-camera-fade-end", "100",
            "--aa", "smaa",
        };
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(enabled)), enabled);
        expect(parsed && parsed.options.motion_blur.mode == Mode::all &&
                   parsed.options.motion_blur.quality ==
                       vcr::MotionBlurQuality::low &&
                   parsed.options.motion_blur.shutter ==
                       vcr::MotionBlurShutter::trailing &&
                   parsed.options.motion_blur.shutter_angle_degrees == 270.0F &&
                   parsed.options.motion_blur.maximum_radius_pixels == 24.0F &&
                   parsed.options.motion_blur.camera_fade_start == 25.0F &&
                   parsed.options.motion_blur.camera_fade_end == 100.0F,
               "motion blur parses bounded quality, shutter and camera controls");
        expect(vcr::CliOptions{}.motion_blur.mode == Mode::off &&
                   vcr::cli_usage("vcr").find("--motion-blur") != std::string::npos,
               "motion blur stays default-off and is documented by CLI usage");

        const char* msaa[]{"vcr", "--aa", "msaa4x", "--motion-blur", "all"};
        const char* t2x[]{"vcr", "--aa", "smaa", "--smaa-mode", "t2x",
                           "--motion-blur", "camera"};
        const char* scaled_fsr[]{"vcr", "--aa", "fsr3", "--fsr-quality", "quality",
                                  "--motion-blur", "objects"};
        const char* native_fsr[]{"vcr", "--aa", "fsr3", "--fsr-quality", "native-aa",
                                  "--motion-blur", "objects"};
        const char* bad_radius[]{"vcr", "--motion-blur-radius", "65"};
        const char* bad_fade[]{"vcr", "--motion-blur-camera-fade-start", "100",
                                "--motion-blur-camera-fade-end", "25"};
        expect(!vcr::parse_cli_options(static_cast<int>(std::size(msaa)), msaa) &&
                   !vcr::parse_cli_options(static_cast<int>(std::size(t2x)), t2x) &&
                   !vcr::parse_cli_options(
                       static_cast<int>(std::size(scaled_fsr)), scaled_fsr) &&
                   vcr::parse_cli_options(
                       static_cast<int>(std::size(native_fsr)), native_fsr) &&
                   !vcr::parse_cli_options(
                       static_cast<int>(std::size(bad_radius)), bad_radius) &&
                   !vcr::parse_cli_options(
                       static_cast<int>(std::size(bad_fade)), bad_fade),
               "motion blur rejects unsupported AA contracts and invalid bounds");
    }
    {
        using Depth = vcr::ShadowDepthFormat;
        const std::array formats{Depth::automatic, Depth::d32, Depth::d16};
        const std::array<const char*, 3> tokens{"auto", "d32", "d16"};
        for (std::size_t i = 0; i < formats.size(); ++i) {
            const char* args[]{"vcr", "--shadow-depth", tokens[i]};
            const auto parsed = vcr::parse_cli_options(3, args);
            expect(parsed && parsed.options.lighting.shadow_depth == formats[i] &&
                       parsed.options.lighting_overrides.shadow_depth &&
                       vcr::to_token(formats[i]) == tokens[i] &&
                       vcr::parse_shadow_depth_format(tokens[i]) == formats[i],
                   "shadow depth tokens and startup CLI profiles parse");
            auto settings = vcr::LightingSettings{};
            settings.shadow_depth = formats[i];
            expect(vcr::valid_lighting_settings(settings), "known shadow depth profile is valid");
        }
        for (const char* bad : {"", "D16", "d24", "automatic", "16", "d32 "}) {
            const char* args[]{"vcr", "--shadow-depth", bad};
            expect(!vcr::parse_cli_options(3, args) && !vcr::parse_shadow_depth_format(bad),
                   "unknown shadow depth profile is rejected strictly");
        }
        const char* missing[]{"vcr", "--shadow-depth"};
        expect(!vcr::parse_cli_options(2, missing), "missing shadow depth profile is rejected");
        auto authored = vcr::LightingSettings{};
        authored.shadow_depth = Depth::d16;
        const vcr::CliOptions defaults{};
        expect(defaults.lighting.shadow_depth == Depth::automatic &&
                   !defaults.lighting_overrides.shadow_depth &&
                   vcr::resolve_scene_lighting(defaults, authored).shadow_depth == Depth::d16,
               "omitted CLI shadow depth preserves the authored startup profile");
        const char* automatic[]{"vcr", "--shadow-depth=auto"};
        const auto override_auto = vcr::parse_cli_options(2, automatic);
        expect(override_auto &&
                   vcr::resolve_scene_lighting(override_auto.options, authored).shadow_depth == Depth::automatic,
               "explicit auto shadow depth overrides an authored d16 profile");
        authored.shadow_depth = static_cast<Depth>(255);
        expect(!vcr::valid_lighting_settings(authored), "unknown shadow depth enum is invalid");
    }
    {
        const char* args[]{"vcr", "--shadow-cache", "cascade"};
        const auto parsed = vcr::parse_cli_options(3, args);
        expect(parsed && parsed.options.lighting.cascade_cache,
               "independent shadow cascade cache CLI");
        auto authored = vcr::LightingSettings{};
        authored.cascade_cache = false;
        expect(vcr::resolve_scene_lighting(parsed.options, authored).cascade_cache,
               "explicit cascade cache overrides authored settings");
        const char* invalid[]{"vcr", "--shadow-cache", "on"};
        expect(!vcr::parse_cli_options(3, invalid), "unknown shadow cache policy rejected");
    }
    {
        const char* mapper_args[]{"vcr", "--tone-mapper", "gt7"};
        const auto parsed = vcr::parse_cli_options(3, mapper_args);
        expect(parsed && parsed.options.tone_mapper == vcr::ToneMapper::gt7,
               "GT7 tone mapper CLI parses");
        const char* invalid_args[]{"vcr", "--tone-mapper", "GT7"};
        expect(!vcr::parse_cli_options(3, invalid_args), "unknown tone mapper rejected");
        const char* missing_args[]{"vcr", "--tone-mapper"};
        expect(!vcr::parse_cli_options(2, missing_args), "missing tone mapper rejected");
        expect(vcr::CliOptions{}.tone_mapper == vcr::ToneMapper::reinhard,
               "Reinhard baseline remains default");
        expect(vcr::resolve_scene_tone_mapper(vcr::CliOptions{}, vcr::ToneMapper::gt7) ==
                   vcr::ToneMapper::gt7 &&
                   vcr::resolve_scene_tone_mapper(parsed.options, vcr::ToneMapper::reinhard) ==
                       vcr::ToneMapper::gt7,
               "authored tone mapper is used unless CLI explicitly overrides it");
    }
    {
        const char* arguments[]{
            "vcr", "--exposure=auto", "--exposure-metering", "center",
            "--exposure-ev", "1.5", "--exposure-min-ev=-8", "--exposure-max-ev", "6",
            "--exposure-low-percent", "0.03", "--exposure-high-percent=.97",
            "--exposure-brighten-seconds", "1.25", "--exposure-darken-seconds=.2",
            "--grading", "tetrahedral", "--grading-lut-size=64",
        };
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(parsed && parsed.options.exposure.mode == vcr::ExposureMode::automatic &&
                   parsed.options.exposure.metering == vcr::ExposureMetering::center &&
                   parsed.options.exposure.compensation_ev == 1.5F &&
                   parsed.options.exposure.minimum_ev == -8.0F &&
                   parsed.options.exposure.maximum_ev == 6.0F &&
                   parsed.options.exposure.low_percentile == 0.03F &&
                   parsed.options.exposure.high_percentile == 0.97F &&
                   parsed.options.exposure.brighten_seconds == 1.25F &&
                   parsed.options.exposure.darken_seconds == 0.2F &&
                   parsed.options.color_grading == vcr::ColorGrading::tetrahedral &&
                   parsed.options.color_grading_lut_size == vcr::ColorGradingLutSize::size_64,
               "display-mapping CLI parses a complete auto-exposure and LUT configuration");
        const char* invalid_range[]{"vcr", "--exposure-low-percent=.98", "--exposure-high-percent=.02"};
        const char* invalid_number[]{"vcr", "--exposure-ev", "nan"};
        const char* invalid_grading[]{"vcr", "--grading", "tetra"};
        const char* invalid_size[]{"vcr", "--grading-lut-size", "16"};
        expect(!vcr::parse_cli_options(3, invalid_range) &&
                   !vcr::parse_cli_options(3, invalid_number) &&
                   !vcr::parse_cli_options(3, invalid_grading) &&
                   !vcr::parse_cli_options(3, invalid_size),
               "display-mapping CLI rejects invalid ranges, non-finite values and unknown LUT modes");
        expect(vcr::CliOptions{}.exposure.mode == vcr::ExposureMode::fixed &&
                   vcr::CliOptions{}.color_grading == vcr::ColorGrading::off,
               "fixed exposure and disabled grading preserve the baseline defaults");
    }
    {
        const auto reversed = vcr::camera_projection(16.0F / 9.0F);
        const auto conventional = glm::perspectiveRH_ZO(
            glm::radians(vcr::camera_vertical_fov), 16.0F / 9.0F,
            vcr::camera_near, vcr::camera_far);
        const auto depth = [](const glm::mat4& matrix, float distance) {
            const auto clip = matrix * glm::vec4(0, 0, -distance, 1);
            return clip.z / clip.w;
        };
        expect(close_to(depth(reversed, vcr::camera_near), 1, 1e-6) &&
                   close_to(depth(reversed, vcr::camera_far), 0, 1e-7),
               "reverse-Z maps near to one and far to zero");
        float previous = 1.0F;
        for (float distance : {0.2F, 1.0F, 10.0F, 100.0F, 400.0F}) {
            const float value = depth(reversed, distance);
            expect(value > 0 && value < previous, "reverse-Z is monotonic");
            previous = value;
        }
        const auto quantization_step = [&](const glm::mat4& matrix, float toward) {
            const auto value = depth(matrix, 400.0F);
            const auto adjacent = std::nextafter(value, toward);
            const auto decode = [&](float encoded) {
                return double(matrix[3][2]) / (double(encoded) + double(matrix[2][2]));
            };
            return std::abs(decode(value) - decode(adjacent));
        };
        const double ordinary_step = quantization_step(conventional, 0.0F);
        const double reversed_step = quantization_step(reversed, 1.0F);
        expect(reversed_step < ordinary_step * 0.001,
               "float reverse-Z improves representable distance spacing at 400m");
        std::cout << "D32 depth spacing at 400m: conventional=" << ordinary_step
                  << "m reverse=" << reversed_step << "m\n";
        const auto clip = reversed * glm::vec4(0.7F, 0.2F, -400.0F, 1.0F);
        auto world = glm::inverse(reversed) * (clip / clip.w);
        world /= world.w;
        expect(close_to(world.z, -400.0F, 0.001) && close_to(world.x, 0.7F, 1e-5),
               "reverse-Z inverse projection preserves distant reconstruction");
    }
    {
        vcr::BenchmarkFrameSamples samples;
        samples.prepare(10'000U);
        for (std::size_t index = 0; index < 10'000U; ++index) {
            samples.record(index < 100U ? 100.0 : 1.0);
        }
        expect(samples.complete() && samples.size() == 10'000U,
               "full benchmark sample interval retained");
        expect(close_to(samples.one_percent_low_fps(), 10.0, 0.001),
               "early hitches survive past the rolling window");
        samples.record(1.0);
        expect(!samples.complete() && samples.size() == 10'000U &&
                   samples.one_percent_low_fps() == 0.0,
               "sample overflow invalidates instead of truncating percentile");
        samples.prepare(4U);
        samples.record(2.0);
        samples.record(4.0);
        expect(samples.complete() && samples.size() == 2U &&
                   close_to(samples.one_percent_low_fps(), 250.0, 0.001),
               "sample reset and sub-100-frame percentile");
        samples.record(std::numeric_limits<double>::quiet_NaN());
        expect(!samples.complete(), "invalid benchmark sample is rejected");
    }
    {
        constexpr std::uint32_t width = 1920U;
        constexpr std::uint32_t height = 1080U;
        const auto scale = vcr::ndc_motion_to_pixel_scale(width, height);
        // A quarter-width and half-height move in pixels corresponds to
        // NDC deltas 0.5 and 1.0, not UV deltas of those sizes.
        expect(close_to(0.5F * scale.x, 480.0, 0.001) &&
                   close_to(1.0F * scale.y, 540.0, 0.001),
               "FSR motion vectors convert from NDC to pixels");
        const vcr::TemporalOffset jitter{0.25F, -0.375F};
        auto projection = vcr::camera_projection(float(width) / float(height));
        projection[1][1] *= -1.0F;
        const glm::vec4 point{0.7F, 0.2F, -5.0F, 1.0F};
        const glm::vec4 before = projection * point;
        const auto offset = vcr::vulkan_projection_jitter(jitter, width, height);
        projection[2][0] += offset.x;
        projection[2][1] += offset.y;
        const glm::vec4 after = projection * point;
        const auto expected_pixels = vcr::vulkan_pixel_jitter(jitter);
        expect(close_to((after.x / after.w - before.x / before.w) * scale.x,
                        expected_pixels.x, 0.0001) &&
                   close_to((after.y / after.w - before.y / before.w) * scale.y,
                            expected_pixels.y, 0.0001),
               "FSR jitter matches actual Vulkan viewport pixel displacement");
    }
    {
        {
            const char *lighting_args[]{"vcr",   "--lighting",    "dynamic", "--gi",
                                        "voxel", "--time-of-day", "0",       "--day-cycle",
                                        "off",   "--day-length",  "120",     "--gi-triangle-budget",
                                        "1024"};
            const auto parsed =
                vcr::parse_cli_options(static_cast<int>(std::size(lighting_args)), lighting_args);
            expect(static_cast<bool>(parsed) && parsed.options.lighting.hour == 0 &&
                       !parsed.options.lighting.cycle &&
                       parsed.options.lighting.triangle_budget == 1024,
                   "dynamic lighting CLI controls parse");
            for (const char *bad :
                 {"24", "-1", "nan", "inf", "9999999999999999999999999999999999999"}) {
                const char *args[]{"vcr", "--time-of-day", bad};
                expect(!vcr::parse_cli_options(3, args), "invalid day hour rejected");
            }
            for (const char *bad : {"1023", "262145", "1024.5", "nan", "-1"}) {
                const char *args[]{"vcr", "--gi-triangle-budget", bad};
                expect(!vcr::parse_cli_options(3, args),
                       "invalid geometry budget rejected before cast");
            }
            for (const char *size : {"1024", "2048", "4096", "8192"}) {
                const char *args[]{"vcr", "--shadow-resolution", size};
                const auto shadow = vcr::parse_cli_options(3, args);
                expect(static_cast<bool>(shadow) &&
                           shadow.options.lighting.shadow_resolution == std::strtoul(size, nullptr, 10),
                       "shadow atlas resolution parses without losing the requested size");
            }
            for (const char *bad : {"0", "512", "1023", "2049", "16384", "2048.5",
                                    "-1", "nan", "4294967296"}) {
                const char *args[]{"vcr", "--shadow-resolution", bad};
                expect(!vcr::parse_cli_options(3, args), "invalid shadow resolution rejected");
            }
        }
        const char *arguments[]{"vulkan_cube"};
        const auto parsed =
            vcr::parse_cli_options(static_cast<int>(std::size(arguments)), arguments);
        expect(
            static_cast<bool>(parsed) &&
                parsed.options.width == 1920 &&
                parsed.options.height == 1080,
            "default drawable resolution is Full HD");
    }

    {
        vcr::PbrMaterial material{
            .base_color = glm::vec4(
                -1.0F,
                0.4F,
                2.0F,
                std::numeric_limits<float>::quiet_NaN()),
            .metallic = 4.0F,
            .roughness = 0.0F,
            .ambient_occlusion = -2.0F,
            .reserved = 7.0F,
        };
        material = vcr::sanitize_pbr_material(material);
        expect(
            material.base_color ==
                    glm::vec4(0.0F, 0.4F, 1.0F, 1.0F) &&
                material.metallic == 1.0F &&
                material.roughness ==
                    vcr::minimum_pbr_roughness &&
                material.ambient_occlusion == 0.0F &&
                material.reserved == 0.0F,
            "PBR material factors are finite and bounded");
    }

    {
        const char* arguments[]{
            "vulkan_cube",
            "--benchmark",
            "--width=1920",
            "--height",
            "1080",
            "--instances",
            "10000",
            "--present",
            "immediate",
            "--aa",
            "fsr3",
            "--fsr-quality=balanced",
            "--smaa-quality",
            "ultra",
            "--no-validation",
        };
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(static_cast<bool>(parsed), "valid CLI parses");
        expect(parsed.options.benchmark, "benchmark flag");
        expect(parsed.options.width == 1920, "width value");
        expect(parsed.options.height == 1080, "height value");
        expect(parsed.options.instances == 10'000, "instance value");
        expect(
            parsed.options.initial_mode == vcr::RunMode::instancing,
            "instances infer instancing mode");
        expect(
            parsed.options.present == vcr::PresentPreference::immediate,
            "present preference");
        expect(
            parsed.options.anti_aliasing.method ==
                    vcr::AntiAliasingMethod::fsr3 &&
                parsed.options.anti_aliasing.fsr3_quality ==
                    vcr::Fsr3Quality::balanced &&
                parsed.options.anti_aliasing.smaa_quality ==
                    vcr::SmaaQuality::ultra,
            "anti-aliasing CLI values");
        expect(!parsed.options.validation, "no-validation flag");
    }

    {
        const char* arguments[]{"vulkan_cube", "--mcp-stdio"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(
            static_cast<bool>(parsed) && parsed.options.mcp_stdio,
            "MCP stdio flag");
    }

    {
        const char* arguments[]{"vulkan_cube", "--width", "0"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(!static_cast<bool>(parsed), "zero width is rejected");
    }

    {
        const char* arguments[]{"vulkan_cube", "--aa", "txaa"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(
            !static_cast<bool>(parsed),
            "unknown anti-aliasing method is rejected");
    }

    {
        const char* two_arguments[]{"vulkan_cube", "--aa", "msaa2x"};
        const char* four_arguments[]{"vulkan_cube", "--aa=msaa4x"};
        const auto two = vcr::parse_cli_options(
            static_cast<int>(std::size(two_arguments)), two_arguments);
        const auto four = vcr::parse_cli_options(
            static_cast<int>(std::size(four_arguments)), four_arguments);
        expect(two && four &&
                   two.options.anti_aliasing.method == vcr::AntiAliasingMethod::msaa2x &&
                   four.options.anti_aliasing.method == vcr::AntiAliasingMethod::msaa4x &&
                   vcr::to_token(vcr::AntiAliasingMethod::msaa2x) == "msaa2x" &&
                   vcr::to_token(vcr::AntiAliasingMethod::msaa4x) == "msaa4x" &&
                   vcr::anti_aliasing_sample_count(vcr::AntiAliasingMethod::off) == 1U &&
                   vcr::anti_aliasing_sample_count(vcr::AntiAliasingMethod::msaa2x) == 2U &&
                   vcr::anti_aliasing_sample_count(vcr::AntiAliasingMethod::msaa4x) == 4U,
               "forward MSAA CLI tokens and sample counts are stable");
        for (const char* bad : {"msaa", "2", "8x", "MSAA2X", "msaa2x ", " msaa4x"}) {
            const char* args[]{"vulkan_cube", "--aa", bad};
            expect(!vcr::parse_cli_options(3, args),
                   "unknown MSAA tokens are rejected strictly");
        }
        const char* incompatible[]{
            "vulkan_cube", "--aa=msaa4x", "--subsurface=full"};
        const auto rejected = vcr::parse_cli_options(3, incompatible);
        expect(!rejected && rejected.error.find("MSAA") != std::string::npos,
               "startup rejects MSAA with packed full-resolution SSS");
    }
    {
        const char* t2x_arguments[]{
            "vulkan_cube", "--aa", "smaa", "--smaa-mode", "t2x"};
        const char* x1_arguments[]{
            "vulkan_cube", "--aa=smaa", "--smaa-mode=1x"};
        const auto t2x = vcr::parse_cli_options(
            static_cast<int>(std::size(t2x_arguments)), t2x_arguments);
        const auto x1 = vcr::parse_cli_options(
            static_cast<int>(std::size(x1_arguments)), x1_arguments);
        expect(t2x && x1 &&
                   t2x.options.anti_aliasing.method == vcr::AntiAliasingMethod::smaa &&
                   t2x.options.anti_aliasing.smaa_mode == vcr::SmaaMode::t2x &&
                   x1.options.anti_aliasing.smaa_mode == vcr::SmaaMode::x1 &&
                   vcr::to_string(vcr::SmaaMode::x1) == "1x" &&
                   vcr::to_string(vcr::SmaaMode::t2x) == "t2x" &&
                   vcr::to_string(t2x.options.anti_aliasing) == "SMAA T2x" &&
                   vcr::to_string(x1.options.anti_aliasing) == "SMAA 1x" &&
                   vcr::is_temporal_smaa(t2x.options.anti_aliasing) &&
                   !vcr::is_temporal_smaa(x1.options.anti_aliasing),
               "SMAA 1x/T2x CLI tokens and temporal classification are stable");
        for (const char* bad : {"2x", "T2X", "t2x ", " t2x", ""}) {
            const char* args[]{"vulkan_cube", "--smaa-mode", bad};
            expect(!vcr::parse_cli_options(3, args),
                   "unknown SMAA mode tokens are rejected strictly");
        }
        const char* missing[]{"vulkan_cube", "--smaa-mode"};
        expect(!vcr::parse_cli_options(2, missing),
               "SMAA mode requires an argument");
        expect(vcr::cli_usage("vulkan_cube").find("--smaa-mode") !=
                   std::string::npos,
               "CLI help documents SMAA temporal mode");
    }

    {
        expect(
            vcr::adjust_quality(vcr::SmaaQuality::low, -1) ==
                    vcr::SmaaQuality::low &&
                vcr::adjust_quality(vcr::SmaaQuality::high, 1) ==
                    vcr::SmaaQuality::ultra,
            "SMAA quality adjustment is bounded");
        expect(
            vcr::adjust_quality(
                vcr::Fsr3Quality::ultra_performance, 1) ==
                    vcr::Fsr3Quality::ultra_performance &&
                close_to(
                    vcr::fsr3_scale(vcr::Fsr3Quality::performance),
                    2.0,
                    0.0001),
            "FSR 3 quality adjustment and scale");
        expect(
            vcr::to_token(vcr::AntiAliasingMethod::fsr3) == "fsr3",
            "anti-aliasing method has a stable machine token");
    }

    {
        const char* arguments[]{"vulkan_cube", "--mode", "minimal"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(
            static_cast<bool>(parsed) &&
                parsed.options.initial_mode == vcr::RunMode::scene,
            "legacy minimal mode selects the game scene");
    }

    {
        const auto interactive_order = vcr::present_preference_order(
            vcr::PresentPreference::automatic, false);
        expect(
            interactive_order.size == 3 &&
                interactive_order.values[0] ==
                    vcr::PresentPreference::immediate &&
                interactive_order.values[1] ==
                    vcr::PresentPreference::mailbox &&
                interactive_order.values[2] ==
                    vcr::PresentPreference::fifo,
            "automatic present mode is uncapped");
    }

    {
        vcr::PerformanceMetrics metrics;
        vcr::FrameTimings timings{};
        timings.cpu_frame_ms = 10.0;
        timings.fence_wait_ms = 1.0;
        timings.gpu_frame_ms = 4.0;
        for (int index = 0; index < 100; ++index) {
            metrics.record_frame(timings);
        }
        const auto snapshot = metrics.snapshot();
        expect(snapshot.frames_rendered == 100, "frame count");
        expect(close_to(snapshot.average_fps, 100.0, 0.001), "average FPS");
        expect(
            close_to(snapshot.one_percent_low_fps, 100.0, 0.001),
            "one-percent-low FPS");
        expect(
            close_to(snapshot.average.gpu_frame_ms, 4.0, 0.001),
            "GPU average");
        expect(
            close_to(snapshot.average.fence_wait_ms, 1.0, 0.001),
            "fence average");
    }

    {
        vcr::PerformanceMetrics metrics;
        expect(metrics.snapshot().average.gpu_postprocess_ms == vcr::unavailable_timing_ms &&
                   metrics.snapshot().average.gpu_display_ms == vcr::unavailable_timing_ms &&
                   metrics.snapshot().average.gpu_exposure_ms == vcr::unavailable_timing_ms &&
                   metrics.snapshot().average.gpu_subsurface_ms == vcr::unavailable_timing_ms &&
                   metrics.snapshot().average.gpu_light_culling_ms == vcr::unavailable_timing_ms,
               "optional GPU timings start unavailable");
        for (const double sample : {vcr::unavailable_timing_ms, 0.25,
                                   std::numeric_limits<double>::quiet_NaN(), 0.5,
                                   std::numeric_limits<double>::infinity(), 0.0}) {
            vcr::FrameTimings timings{};
            timings.gpu_postprocess_ms = sample;
            timings.gpu_display_ms = sample;
            timings.gpu_exposure_ms = sample;
            timings.gpu_subsurface_ms = sample;
            timings.gpu_light_culling_ms = sample;
            metrics.record_frame(timings);
        }
        const auto snapshot = metrics.snapshot();
        expect(close_to(snapshot.average.gpu_postprocess_ms, 0.25, 0.000001) &&
                   snapshot.latest.gpu_postprocess_ms == 0.0 &&
                   close_to(snapshot.average.gpu_display_ms, 0.25, 0.000001) &&
                   snapshot.latest.gpu_display_ms == 0.0 &&
                   close_to(snapshot.average.gpu_exposure_ms, 0.25, 0.000001) &&
                   snapshot.latest.gpu_exposure_ms == 0.0 &&
                   close_to(snapshot.average.gpu_subsurface_ms, 0.25, 0.000001) &&
                   snapshot.latest.gpu_subsurface_ms == 0.0 &&
                   close_to(snapshot.average.gpu_light_culling_ms, 0.25, 0.000001) &&
                   snapshot.latest.gpu_light_culling_ms == 0.0,
               "optional GPU averages exclude unavailable queries and include a measured zero");
        const auto result = vcr::make_benchmark_result(
            "GPU", "driver", "640x360", "640x360", "IMMEDIATE", "off", "off",
            1, 0, snapshot);
        const auto timing_json = vcr::benchmark_result_to_json(result);
        expect(close_to(result.average_gpu_postprocess_ms, 0.25, 0.000001) &&
                   close_to(result.average_gpu_display_ms, 0.25, 0.000001) &&
                   close_to(result.average_gpu_exposure_ms, 0.25, 0.000001) &&
                   close_to(result.average_gpu_subsurface_ms, 0.25, 0.000001) &&
                   close_to(result.average_gpu_light_culling_ms, 0.25, 0.000001) &&
                   timing_json.find("\"average_gpu_postprocess_ms\": 0.25") != std::string::npos &&
                   timing_json.find("\"average_gpu_display_ms\": 0.25") != std::string::npos &&
                   timing_json.find("\"average_gpu_exposure_ms\": 0.25") != std::string::npos &&
                   timing_json.find("\"average_gpu_subsurface_ms\": 0.25") != std::string::npos &&
                   timing_json.find("\"average_gpu_light_culling_ms\": 0.25") != std::string::npos,
               "benchmark preserves measured optional GPU averages");
        metrics.reset();
        const auto reset = metrics.snapshot();
        expect(reset.latest.gpu_postprocess_ms == vcr::unavailable_timing_ms &&
                   reset.average.gpu_postprocess_ms == vcr::unavailable_timing_ms &&
                   reset.latest.gpu_display_ms == vcr::unavailable_timing_ms &&
                   reset.average.gpu_display_ms == vcr::unavailable_timing_ms &&
                   reset.latest.gpu_exposure_ms == vcr::unavailable_timing_ms &&
                   reset.average.gpu_exposure_ms == vcr::unavailable_timing_ms &&
                   reset.latest.gpu_subsurface_ms == vcr::unavailable_timing_ms &&
                   reset.average.gpu_subsurface_ms == vcr::unavailable_timing_ms &&
                   reset.latest.gpu_light_culling_ms == vcr::unavailable_timing_ms &&
                   reset.average.gpu_light_culling_ms == vcr::unavailable_timing_ms,
               "metrics reset clears latest and average optional GPU timings");
    }

    {
        vcr::PerformanceMetrics metrics;
        expect(metrics.snapshot().average.gpu_depth_prepass_ms == vcr::unavailable_timing_ms,
               "depth prepass timing starts unavailable");
        for (const double sample : {vcr::unavailable_timing_ms, 0.125,
                                   std::numeric_limits<double>::quiet_NaN(), 0.625,
                                   std::numeric_limits<double>::infinity(), 0.0}) {
            vcr::FrameTimings timings{};
            timings.gpu_postprocess_ms = 1.0;
            timings.gpu_depth_prepass_ms = sample;
            metrics.record_frame(timings);
        }
        const auto snapshot = metrics.snapshot();
        expect(close_to(snapshot.average.gpu_depth_prepass_ms, 0.25, 0.000001) &&
                   snapshot.latest.gpu_depth_prepass_ms == 0.0 &&
                   snapshot.average.gpu_postprocess_ms == 1.0,
               "prepass average excludes unavailable queries, includes disabled zero and remains independent");
        auto result = vcr::make_benchmark_result(
            "GPU", "driver", "640x360", "640x360", "IMMEDIATE", "off", "off",
            1, 0, snapshot);
        expect(close_to(result.average_gpu_depth_prepass_ms, 0.25, 0.000001) &&
                   vcr::benchmark_result_to_json(result).find(
                       "\"average_gpu_depth_prepass_ms\": 0.25") != std::string::npos,
               "benchmark preserves the measured prepass average");
        expect(result.depth_prepass_requested == "off" && result.depth_prepass_actual == "off",
               "benchmark depth prepass policy defaults to off");
        result.depth_prepass_requested = "partial";
        result.depth_prepass_actual = "off";
        const auto policy_json = vcr::benchmark_result_to_json(result);
        expect(policy_json.find("\"depth_prepass_requested\": \"partial\"") != std::string::npos &&
                   policy_json.find("\"depth_prepass_actual\": \"off\"") != std::string::npos,
               "benchmark serializes requested and actual prepass policies independently");
        metrics.reset();
        const auto reset = metrics.snapshot();
        expect(reset.latest.gpu_depth_prepass_ms == vcr::unavailable_timing_ms &&
                   reset.average.gpu_depth_prepass_ms == vcr::unavailable_timing_ms,
               "metrics reset clears latest and average prepass timings");
        const auto reset_result = vcr::make_benchmark_result(
            "GPU", "driver", "640x360", "640x360", "IMMEDIATE", "off", "off",
            1, 0, reset);
        expect(vcr::benchmark_result_to_json(reset_result).find(
                   "\"average_gpu_depth_prepass_ms\": null") != std::string::npos,
               "unavailable prepass timing serializes as null");
        vcr::FrameTimings disabled{};
        disabled.gpu_depth_prepass_ms = 0.0;
        metrics.record_frame(disabled);
        const auto disabled_result = vcr::make_benchmark_result(
            "GPU", "driver", "640x360", "640x360", "IMMEDIATE", "off", "off",
            1, 0, metrics.snapshot());
        expect(disabled_result.average_gpu_depth_prepass_ms == 0.0 &&
                   vcr::benchmark_result_to_json(disabled_result).find(
                       "\"average_gpu_depth_prepass_ms\": 0,") != std::string::npos,
               "disabled prepass timing remains an available zero in benchmark JSON");
    }

    {
        using namespace std::chrono_literals;
        vcr::BenchmarkController controller(1.0, 2.0);
        const auto start = vcr::BenchmarkController::TimePoint{};
        controller.start(start);
        auto tick = controller.tick(start + 500ms);
        expect(
            tick.phase == vcr::BenchmarkPhase::warmup,
            "benchmark warmup phase");
        tick = controller.tick(start + 1s);
        expect(tick.measurement_began, "measurement transition");
        tick = controller.tick(start + 3s);
        expect(tick.completed, "benchmark completion");
    }

    {
        vcr::BenchmarkResult result{};
        result.gpu = "GPU \"quoted\"";
        result.render_resolution = "1280x720";
        result.anti_aliasing = "FSR 3.1.4";
        result.anti_aliasing_quality = "quality";
        result.anti_aliasing_requested = "msaa4x";
        result.anti_aliasing_actual = "msaa2x";
        result.smaa_mode = "t2x";
        result.smaa_temporal_samples = 2;
        result.smaa_history_resident_bytes = 33'177'600;
        result.smaa_history_incremental_bytes = 0;
        result.smaa_history_reuses_fsr_output = true;
        result.msaa_samples_requested = 4;
        result.msaa_samples_actual = 2;
        result.msaa_2x_supported = true;
        result.msaa_4x_supported = false;
        result.msaa_resident_bytes = 99'614'720;
        result.render_fixture = "motion-bars";
        result.motion_fixture_fps = 60U;
        result.frames_rendered = 42;
        result.mode = "scene";
        result.measurement_seconds = 15.0;
        result.percentile_samples = 42;
        result.average_gpu_workload_ms = vcr::unavailable_timing_ms;
        result.local_light_backend_requested = "clustered";
        result.local_light_backend_actual = "clustered";
        result.local_light_count = 256;
        result.local_light_assignments = 1024;
        result.local_light_maximum_list = 12;
        result.local_light_overflow_clusters = 0;
        result.local_light_dropped = 0;
        result.exposure.compensation_ev = -1.5F;
        result.exposure.minimum_ev = -12.0F;
        result.exposure.maximum_ev = -2.0F;
        const std::string json = vcr::benchmark_result_to_json(result);
        expect(
            json.find("GPU \\\"quoted\\\"") != std::string::npos,
            "JSON string escaping");
        expect(
            json.find("\"frames_rendered\": 42") != std::string::npos,
            "JSON integer field");
        expect(
            json.find("\"anti_aliasing\": \"FSR 3.1.4\"") !=
                    std::string::npos &&
                json.find("\"render_resolution\": \"1280x720\"") !=
                    std::string::npos,
            "benchmark records anti-aliasing and render resolution");
        expect(json.find("\"anti_aliasing_requested\": \"msaa4x\"") != std::string::npos &&
                   json.find("\"anti_aliasing_actual\": \"msaa2x\"") != std::string::npos &&
                   json.find("\"smaa_mode\": \"t2x\"") != std::string::npos &&
                   json.find("\"smaa_temporal_samples\": 2") != std::string::npos &&
                   json.find("\"smaa_history_resident_bytes\": 33177600") != std::string::npos &&
                   json.find("\"smaa_history_incremental_bytes\": 0") != std::string::npos &&
                   json.find("\"smaa_history_reuses_fsr_output\": true") != std::string::npos &&
                   json.find("\"msaa_samples_requested\": 4") != std::string::npos &&
                   json.find("\"msaa_samples_actual\": 2") != std::string::npos &&
                   json.find("\"msaa_2x_supported\": true") != std::string::npos &&
                   json.find("\"msaa_4x_supported\": false") != std::string::npos &&
                   json.find("\"msaa_resident_bytes\": 99614720") != std::string::npos &&
                   json.find("\"render_fixture\": \"motion-bars\"") != std::string::npos &&
                   json.find("\"motion_fixture_fps\": 60") != std::string::npos,
               "benchmark serializes AA state and deterministic fixture rate");
        expect(json.find("\"percentile_scope\": \"full_measurement\"") != std::string::npos &&
                   json.find("\"average_gpu_workload_ms\": null") != std::string::npos &&
                   json.find("\"average_gpu_postprocess_ms\": null") != std::string::npos &&
                   json.find("\"average_gpu_display_ms\": null") != std::string::npos &&
                   json.find("\"average_gpu_exposure_ms\": null") != std::string::npos &&
                   json.find("\"average_gpu_subsurface_ms\": null") != std::string::npos &&
                   json.find("\"average_gpu_light_culling_ms\": null") != std::string::npos &&
                   json.find("\"exposure_compensation_ev\": -1.5") != std::string::npos &&
                   json.find("\"exposure_minimum_ev\": -12") != std::string::npos &&
                   json.find("\"exposure_maximum_ev\": -2") != std::string::npos &&
                   json.find("\"local_light_backend_actual\": \"clustered\"") != std::string::npos &&
                   json.find("\"local_light_count\": 256") != std::string::npos &&
                   json.find("\"local_light_assignments\": 1024") != std::string::npos &&
                   json.find("\"local_light_maximum_list\": 12") != std::string::npos &&
                   json.find("\"local_light_overflow_clusters\": 0") != std::string::npos &&
                   json.find("\"local_light_dropped\": 0") != std::string::npos &&
                   json.find("\"mode\": \"scene\"") != std::string::npos,
               "benchmark exposes sample scope, optional timings and local-light diagnostics");
    }

    {
        vcr::FlyCamera camera(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        const glm::vec3 forward = camera.forward();
        expect(
            close_to(forward.x, 0.0, 0.0001) &&
                close_to(forward.y, 0.0, 0.0001) &&
                close_to(forward.z, -1.0, 0.0001),
            "camera initially faces negative Z");

        vcr::FlyCameraInput input{};
        input.forward = true;
        camera.update(input, 0.05F);
        expect(
            close_to(camera.position().z, -0.3, 0.0001),
            "camera forward movement uses configured speed");

        vcr::FlyCamera boosted(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        input.boost = true;
        boosted.update(input, 0.05F);
        expect(
            close_to(boosted.position().z, -1.2, 0.0001),
            "camera shift boost multiplies movement speed");
    }

    {
        vcr::FlyCamera forward_only(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        vcr::FlyCamera diagonal(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        vcr::FlyCameraInput input{};
        input.forward = true;
        forward_only.update(input, 0.05F);
        input.right = true;
        diagonal.update(input, 0.05F);
        expect(
            close_to(
                glm::length(forward_only.position()),
                glm::length(diagonal.position()),
                0.0001),
            "camera diagonal movement is normalized");

        const glm::vec3 before = diagonal.forward();
        input = {};
        input.mouse_delta_x = 100.0F;
        diagonal.update(input, 0.0F);
        expect(
            glm::length(diagonal.forward() - before) > 0.01F,
            "mouse look is independent of frame delta");
    }

    {
        vcr::FlyCamera camera(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        vcr::FlyCameraInput input{};
        input.forward = true;
        input.backward = true;
        input.left = true;
        input.right = true;
        input.up = true;
        input.down = true;
        camera.update(input, 0.05F);
        expect(
            glm::length(camera.position()) < 0.0001F,
            "opposing camera inputs cancel");

        input = {};
        input.forward = true;
        camera.update(input, 1.0F);
        expect(
            close_to(camera.position().z, -0.3, 0.0001),
            "camera delta is clamped after a stall");
        const glm::vec3 position = camera.position();
        camera.update(input, -1.0F);
        camera.update(
            input, std::numeric_limits<float>::quiet_NaN());
        expect(
            glm::length(camera.position() - position) < 0.0001F,
            "invalid camera deltas do not move the camera");

        input = {};
        input.mouse_delta_y = 100'000.0F;
        camera.update(input, 0.0F);
        expect(
            camera.forward().y < -0.99F,
            "camera pitch is clamped below the singularity");
    }

    {
        vcr::McpControlPlane control;
        vcr::McpCommand submitted{
            .token = 42,
            .type = vcr::McpCommandType::render_set_instances,
            .value = 10'000,
        };
        expect(
            control.submit(submitted),
            "MCP command queue accepts a command");
        vcr::McpCommand received{};
        expect(
            control.try_next_command(received) &&
                received.token == 42 &&
                received.value == 10'000,
            "MCP command queue preserves typed payload");
        expect(
            control.complete(vcr::McpCommandResult{
                .token = 42,
                .success = true,
                .message = "ok",
                .structured_json = "{\"ok\":true}",
            }),
            "MCP result queue accepts a response");
        vcr::McpCommandResult result{};
        expect(
            control.try_next_result(result) &&
                result.token == 42 &&
                result.success &&
                result.structured_json == "{\"ok\":true}",
            "MCP result queue preserves ownership and ordering");
    }

    {
        constexpr std::uint64_t command_count = 100'000;
        vcr::McpControlPlane control;
        std::atomic<bool> producer_failed{false};
        std::jthread producer([&]() {
            for (std::uint64_t token = 1;
                 token <= command_count;
                 ++token) {
                vcr::McpCommand command{
                    .token = token,
                    .type = vcr::McpCommandType::debug_get_state,
                };
                while (!control.submit(command)) {
                    std::this_thread::yield();
                }
                vcr::McpCommandResult result{};
                while (!control.try_next_result(result)) {
                    std::this_thread::yield();
                }
                if (!result.success || result.token != token) {
                    producer_failed.store(
                        true, std::memory_order_relaxed);
                    return;
                }
            }
        });

        bool ordering_failed = false;
        for (std::uint64_t expected = 1;
             expected <= command_count;
             ++expected) {
            vcr::McpCommand command{};
            while (!control.try_next_command(command)) {
                std::this_thread::yield();
            }
            if (command.token != expected) {
                ordering_failed = true;
            }
            vcr::McpCommandResult result{
                .token = command.token,
                .success = true,
                .message = "ok",
                .structured_json = "{\"ok\":true}",
            };
            while (!control.complete(std::move(result))) {
                std::this_thread::yield();
            }
        }
        producer.join();
        expect(
            !ordering_failed &&
                !producer_failed.load(std::memory_order_relaxed),
            "MCP SPSC queues preserve ordering under concurrent stress");
    }

    {
        constexpr std::array<std::uint8_t, 8> pixels{
            255U, 0U, 0U, 255U,
            0U, 255U, 0U, 255U,
        };
        const auto png = vcr::encode_png_rgba(pixels, 2, 1);
        expect(static_cast<bool>(png), "PNG encoder accepts RGBA8 pixels");
        constexpr std::array<std::uint8_t, 8> signature{
            0x89U, 0x50U, 0x4eU, 0x47U,
            0x0dU, 0x0aU, 0x1aU, 0x0aU,
        };
        expect(
            png.bytes.size() > 33U &&
                std::equal(
                    signature.begin(),
                    signature.end(),
                    png.bytes.begin()),
            "PNG signature");
        expect(
            png.bytes.size() > 24U &&
                png.bytes[12] == 'I' &&
                png.bytes[13] == 'H' &&
                png.bytes[14] == 'D' &&
                png.bytes[15] == 'R' &&
                png.bytes[19] == 2U &&
                png.bytes[23] == 1U &&
                png.bytes[24] == 8U &&
                png.bytes[25] == 6U,
            "PNG IHDR dimensions and RGBA color type");
        const auto invalid = vcr::encode_png_rgba(pixels, 0, 1);
        expect(
            !static_cast<bool>(invalid),
            "PNG encoder rejects zero dimensions");
    }

    {
        expect(
            vcr::valid_scene_asset_path(
                "models/tree/tree.vcrasset.json") &&
                vcr::valid_scene_asset_path(
                    "import/"
                    "\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB5"
                    "\xD0\xB2\xD0\xBE.vcrasset.json"),
            "scene model asset paths accept bounded relative UTF-8 paths");
        std::string embedded_nul = "model.vcrasset.json";
        embedded_nul.push_back('\0');
        embedded_nul += "hidden";
        std::string invalid_utf8 = "model/";
        invalid_utf8.append("\xC0\xAF", 2U);
        invalid_utf8 += ".vcrasset.json";
        expect(
            !vcr::valid_scene_asset_path(
                "../model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "/model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "C:/model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "folder\\model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "model.obj") &&
                !vcr::valid_scene_asset_path(embedded_nul) &&
                !vcr::valid_scene_asset_path(invalid_utf8),
            "scene model asset paths reject escape and malformed values");

        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        const vcr::SceneNodeId child = scene.create_node(
            "Child", vcr::SceneNodeKind::folder, root);
        const vcr::SceneNodeId cube = scene.create_node(
            "Cube", vcr::SceneNodeKind::cube, child);
        expect(
            scene.valid(root) && scene.valid(child) &&
                scene.valid(cube) && scene.size() == 3U,
            "scene creates stable hierarchical nodes");
        expect(
            !scene.reparent(root, cube),
            "scene rejects a hierarchy cycle");
        expect(
            !scene.create_node(
                std::string(
                    vcr::max_scene_node_name_bytes + 1U,
                    'x'),
                vcr::SceneNodeKind::folder),
            "scene rejects an unbounded node name");
        const std::uint64_t render_revision =
            scene.content_revision();
        const std::uint64_t hierarchy_revision =
            scene.structure_revision();
        expect(
            scene.rename_node(cube, "Renamed cube") &&
                scene.content_revision() == render_revision &&
                scene.structure_revision() ==
                    hierarchy_revision + 1U,
            "scene rename invalidates hierarchy without GPU content");

        std::vector<vcr::SceneHierarchyRow> rows;
        rows.reserve(16U);
        scene.build_hierarchy_rows(rows);
        expect(
            rows.size() == 3U &&
                rows[0].depth == 0U &&
                rows[1].depth == 1U &&
                rows[2].depth == 2U,
            "scene hierarchy cache is iterative preorder");

        expect(
            scene.destroy_subtree(child) &&
                !scene.valid(child) &&
                !scene.valid(cube) &&
                scene.valid(root) &&
                scene.size() == 1U,
            "scene subtree deletion invalidates stale handles");
        const vcr::SceneNodeId reused = scene.create_node(
            "Reused", vcr::SceneNodeKind::cube, root);
        expect(
            reused.index == child.index &&
                reused.generation != child.generation &&
                !scene.valid(child),
            "scene slot reuse increments generation");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        static_cast<void>(scene.create_node(
            "Terrain", vcr::SceneNodeKind::terrain, root));
        static_cast<void>(scene.create_node(
            "Sky", vcr::SceneNodeKind::environment, root));
        static_cast<void>(scene.create_node(
            "Sun", vcr::SceneNodeKind::directional_light, root));
        const auto ocean_parent = scene.create_node(
            "Sea level", vcr::SceneNodeKind::folder, root);
        const auto ocean = scene.create_node(
            "Ocean", vcr::SceneNodeKind::ocean, ocean_parent);
        scene.node(ocean_parent)->get().transform.translation.y = -1.0F;
        scene.node(ocean_parent)->get().transform.scale.y = 2.0F;
        scene.node(ocean)->get().transform.translation.y = -1.0F;
        scene.node(ocean)->get().ocean.wave_height = 2.0F;
        scene.node(ocean)->get().ocean.wave_direction_degrees = 135.0F;
        static_cast<void>(scene.create_node(
            "Cube", vcr::SceneNodeKind::cube, root));
        const vcr::SceneNodeId model = scene.create_node(
            "Model", vcr::SceneNodeKind::model, root);
        const vcr::SceneNodeId point = scene.create_node(
            "Point", vcr::SceneNodeKind::point_light, root);
        const vcr::SceneNodeId spot = scene.create_node(
            "Spot", vcr::SceneNodeKind::spot_light, root);
        if (auto model_node = scene.node(model)) {
            model_node->get().asset_path =
                "models/tree/tree.vcrasset.json";
            model_node->get().transform.translation =
                glm::vec3(2.0F, 3.0F, -4.0F);
            scene.mark_content_changed();
        }
        if (auto point_node = scene.node(point)) {
            point_node->get().transform.translation = glm::vec3(1.0F, 2.0F, -3.0F);
            point_node->get().light.intensity_candela = 250.0F;
            scene.mark_content_changed();
        }
        std::vector<vcr::SceneGpuInstance> instances;
        std::vector<vcr::SceneModelInstance> models;
        std::vector<vcr::ScenePunctualLight> lights;
        const vcr::SceneRenderView visible =
            scene.build_render_view(instances, models, lights);
        expect(
            visible.cubes.size() == 1U &&
                visible.models.size() == 1U &&
                visible.models[0].asset_path ==
                    "models/tree/tree.vcrasset.json" &&
                visible.models[0].model_row_0.w == 2.0F &&
                visible.models[0].model_row_1.w == 3.0F &&
                visible.models[0].model_row_2.w == -4.0F &&
                visible.local_lights.size() == 2U &&
                visible.local_lights[0].id == point &&
                visible.local_lights[1].id == spot &&
                visible.local_lights[0].gpu.position_range ==
                    glm::vec4(1.0F, 2.0F, -3.0F, 10.0F) &&
                visible.local_lights[0].gpu.color_intensity.w == 250.0F &&
                visible.terrain_visible &&
                visible.sky_visible &&
                visible.sun_visible && visible.ocean_visible &&
                visible.ocean_height == -3.0F &&
                visible.ocean == scene.node(ocean)->get().ocean,
            "scene render view exposes visible component flags");
        scene.node(ocean)->get().ocean.wave_speed = 0.0F;
        scene.mark_content_changed();
        expect(scene.build_render_view(instances, models, lights).ocean.wave_speed == 0.0F,
               "ocean settings update independently of cached transforms");
        scene.node(ocean)->get().ocean.wave_length = 0.0F;
        expect(!scene.build_render_view(instances, models, lights).ocean_visible,
               "invalid ocean settings never reach the renderer");
        scene.node(ocean)->get().ocean = {};
        if (auto root_node = scene.node(root)) {
            root_node->get().visible = false;
            scene.mark_content_changed();
        }
        const vcr::SceneRenderView hidden =
            scene.build_render_view(instances, models, lights);
        expect(
            hidden.cubes.empty() &&
                hidden.models.empty() &&
                hidden.local_lights.empty() &&
                !hidden.terrain_visible &&
                !hidden.sky_visible &&
                !hidden.sun_visible && !hidden.ocean_visible,
            "hidden parent suppresses renderable descendants");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId parent = scene.create_node(
            "Scaled parent", vcr::SceneNodeKind::folder);
        const vcr::SceneNodeId cube = scene.create_node(
            "Rotated child", vcr::SceneNodeKind::cube, parent);
        auto parent_node = scene.node(parent);
        auto cube_node = scene.node(cube);
        expect(
            parent_node.has_value() && cube_node.has_value(),
            "scene transform test nodes exist");
        if (parent_node && cube_node) {
            parent_node->get().transform.translation =
                glm::vec3(3.0F, -2.0F, 5.0F);
            parent_node->get().transform.rotation_degrees =
                glm::vec3(12.0F, 31.0F, -7.0F);
            parent_node->get().transform.scale =
                glm::vec3(2.0F, 3.0F, 0.5F);
            cube_node->get().transform.translation =
                glm::vec3(-4.0F, 1.0F, 2.0F);
            cube_node->get().transform.rotation_degrees =
                glm::vec3(27.0F, -19.0F, 43.0F);
            cube_node->get().transform.scale =
                glm::vec3(0.75F, 1.25F, 1.5F);
            scene.mark_content_changed();
        }

        std::vector<vcr::SceneGpuInstance> instances;
        std::vector<vcr::SceneModelInstance> models;
        std::vector<vcr::ScenePunctualLight> lights;
        instances.reserve(1U);
        const vcr::SceneRenderView snapshot =
            scene.build_render_view(instances, models, lights);
        expect(
            snapshot.cubes.size() == 1U,
            "scene transform emits one cube");
        if (snapshot.cubes.size() == 1U &&
            parent_node && cube_node) {
            const auto local = [](const vcr::SceneTransform& value) {
                return glm::translate(
                           glm::mat4(1.0F),
                           value.translation) *
                       glm::mat4_cast(glm::quat(glm::radians(
                           value.rotation_degrees))) *
                       glm::scale(
                           glm::mat4(1.0F), value.scale);
            };
            const glm::mat4 expected =
                local(parent_node->get().transform) *
                local(cube_node->get().transform);
            const vcr::SceneGpuInstance& packed =
                snapshot.cubes.front();
            const std::array rows{
                packed.model_row_0,
                packed.model_row_1,
                packed.model_row_2,
                glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
            };
            bool matches = true;
            for (glm::length_t column = 0;
                 column < 4;
                 ++column) {
                for (glm::length_t row = 0;
                     row < 4;
                     ++row) {
                    matches =
                        matches &&
                        close_to(
                            rows[static_cast<std::size_t>(row)]
                                [column],
                            expected[column][row],
                            0.0001);
                }
            }
            expect(
                matches,
                "hierarchical affine transform preserves shear");
            const auto authored_world =
                vcr::scene_world_matrix(scene, cube);
            expect(
                authored_world.has_value(),
                "editor gizmo resolves selected world transform");
            if (authored_world) {
                const auto recovered =
                    vcr::scene_local_transform_from_world(
                        scene, cube, *authored_world);
                bool round_trip = recovered.has_value();
                if (recovered) {
                    const glm::mat4 recovered_world =
                        vcr::scene_transform_matrix(
                            parent_node->get().transform) *
                        vcr::scene_transform_matrix(*recovered);
                    for (glm::length_t column = 0;
                         column < 4 && round_trip;
                         ++column) {
                        for (glm::length_t row = 0;
                             row < 4;
                             ++row) {
                            round_trip = close_to(
                                recovered_world[column][row],
                                (*authored_world)[column][row],
                                0.001);
                        }
                    }
                }
                expect(
                    round_trip,
                    "gizmo world-to-local conversion preserves hierarchical TRS");
            }
        }
    }

    {
        vcr::SceneDocument scene;
        const auto root = scene.create_node(
            "Group", vcr::SceneNodeKind::folder);
        const auto child = scene.create_node(
            "Cube", vcr::SceneNodeKind::cube, root);
        const auto grandchild = scene.create_node(
            "Model", vcr::SceneNodeKind::model, child);
        scene.node(root)->get().transform.translation =
            {4.0F, 1.0F, -2.0F};
        scene.node(child)->get().material.metallic = 0.65F;
        scene.node(grandchild)->get().asset_path =
            "models/tree/tree.vcrasset.json";
        scene.mark_content_changed();
        std::uint32_t duplicated_nodes = 0U;
        std::string error;
        const auto duplicate = vcr::duplicate_scene_subtree(
            scene, root, duplicated_nodes, error);
        std::vector<vcr::SceneNodeRecord> records;
        scene.build_node_records(records);
        expect(
            duplicate && error.empty() && duplicated_nodes == 3U &&
                scene.size() == 6U &&
                scene.parent(duplicate) == vcr::SceneNodeId{} &&
                scene.node(duplicate)->get().name == "Group Copy" &&
                scene.node(duplicate)->get().transform ==
                    scene.node(root)->get().transform &&
                records.back().value.asset_path ==
                    "models/tree/tree.vcrasset.json",
            "editor duplicate copies a complete authored subtree");
    }

    {
        vcr::SceneDocument scene;
        const auto root = scene.create_node("Collapsed root", vcr::SceneNodeKind::folder);
        const auto child = scene.create_node("Collapsed child", vcr::SceneNodeKind::folder, root);
        const auto leaf = scene.create_node("Hidden leaf", vcr::SceneNodeKind::cube, child);
        const auto sibling = scene.create_node("Sibling", vcr::SceneNodeKind::cube, root);
        const auto other_root = scene.create_node("Other root", vcr::SceneNodeKind::folder);
        scene.set_expanded(root, false);
        scene.set_expanded(child, false);
        scene.node(leaf)->get().visible = false;
        const auto structure_revision = scene.structure_revision();
        const auto content_revision = scene.content_revision();
        std::vector<vcr::SceneHierarchyRow> rows;
        scene.build_hierarchy_rows(rows);
        expect(rows.size() == 2 && rows[0].id == root && rows[1].id == other_root,
               "UI traversal still hides descendants of collapsed groups");
        scene.build_hierarchy_rows(rows, {}, vcr::SceneHierarchyScope::all);
        const std::array expected_ids{root, child, leaf, sibling, other_root};
        const std::array expected_depths{0U, 1U, 2U, 1U, 0U};
        bool complete = rows.size() == scene.size() && rows.size() == expected_ids.size();
        for (std::size_t i = 0; complete && i < rows.size(); ++i)
            complete = rows[i].id == expected_ids[i] && rows[i].depth == expected_depths[i];
        expect(complete, "MCP traversal includes collapsed and invisible descendants in preorder");
        expect(rows.size() >= 3 && rows[0].has_children && rows[1].has_children && !rows[2].has_children,
               "full traversal retains hierarchy metadata for paginated node responses");
        scene.set_expanded(root, true);
        scene.set_expanded(child, true);
        std::vector<vcr::SceneHierarchyRow> expanded;
        scene.build_hierarchy_rows(expanded, {}, vcr::SceneHierarchyScope::all);
        bool stable = expanded.size() == rows.size();
        for (std::size_t i = 0; stable && i < rows.size(); ++i)
            stable = expanded[i].id == rows[i].id && expanded[i].depth == rows[i].depth;
        expect(stable, "expanding the UI does not shift MCP pagination offsets or total");
        scene.set_expanded(root, false);
        scene.set_expanded(child, false);
        const auto collapsed_revision = scene.structure_revision();
        scene.build_hierarchy_rows(rows, {}, vcr::SceneHierarchyScope::all);
        scene.build_hierarchy_rows(expanded);
        expect(expanded.size() == 2 && scene.structure_revision() == collapsed_revision &&
                   scene.content_revision() == content_revision &&
                   collapsed_revision == structure_revision + 4,
               "full enumeration is read-only and does not expand editor groups");
    }

    {
        vcr::SceneDocument scene;
        vcr::SceneNodeId parent = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        constexpr std::uint32_t depth = 10'000U;
        for (std::uint32_t index = 1U; index < depth; ++index) {
            parent = scene.create_node(
                "Node", vcr::SceneNodeKind::folder, parent);
        }
        std::vector<vcr::SceneHierarchyRow> rows;
        rows.reserve(depth);
        scene.build_hierarchy_rows(rows);
        expect(
            rows.size() == depth &&
                rows.back().depth == depth - 1U,
            "deep hierarchy traversal does not recurse");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        for (std::uint32_t index = 1U;
             index < vcr::max_scene_nodes;
             ++index) {
            const vcr::SceneNodeId node = scene.create_node(
                "Node", vcr::SceneNodeKind::folder, root);
            if (!node) {
                break;
            }
        }
        expect(
            scene.size() == vcr::max_scene_nodes &&
                !scene.create_node(
                    "Overflow",
                    vcr::SceneNodeKind::folder),
            "scene node capacity is strictly bounded");
        expect(
            scene.destroy_subtree(root) &&
                scene.size() == 0U,
            "maximum-size subtree destruction is iterative");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Landscape", vcr::SceneNodeKind::folder);
        std::string error;
        const vcr::SceneNodeId ground = vcr::create_flat_ground(
            scene,
            vcr::FlatGroundOptions{
                .name = "Playable Ground",
                .parent = root,
                .center_x = 4.0F,
                .height = -2.0F,
                .center_z = -6.0F,
                .width = 80.0F,
                .depth = 40.0F,
            },
            error);
        const auto ground_node = scene.node(ground);
        expect(
            ground && error.empty() && ground_node &&
                ground_node->get().kind == vcr::SceneNodeKind::ground &&
                close_to(ground_node->get().transform.translation.y, -2.0, 0.0001) &&
                close_to(ground_node->get().transform.scale.x, 40.0, 0.0001) &&
                close_to(ground_node->get().transform.scale.z, 20.0, 0.0001),
            "flat terrain tool creates an explicitly sized ground plane");

        vcr::ModelScatterOptions scatter{
            .asset_path = "plants/fern/fern.vcrasset.json",
            .name_prefix = "Fern",
            .parent = root,
            .base_transform = vcr::SceneTransform{
                .translation = glm::vec3(0.25F, 0.5F, -0.25F),
                .rotation_degrees = glm::vec3(0.0F, 15.0F, 0.0F),
                .scale = glm::vec3(0.5F),
            },
            .center_x = 3.0F,
            .height = -2.0F,
            .center_z = -4.0F,
            .width = 20.0F,
            .depth = 12.0F,
            .minimum_scale = 0.75F,
            .maximum_scale = 1.25F,
            .count = 32U,
            .seed = 42U,
        };
        std::vector<vcr::SceneNodeId> first;
        expect(
            vcr::scatter_model_instances(scene, scatter, first, error) &&
                error.empty() && first.size() == scatter.count,
            "model scatter creates the requested bounded batch");
        std::vector<vcr::SceneTransform> first_transforms;
        first_transforms.reserve(first.size());
        for (const vcr::SceneNodeId id : first) {
            const auto node = scene.node(id);
            if (node) {
                first_transforms.push_back(node->get().transform);
            }
        }

        vcr::SceneDocument repeat;
        const vcr::SceneNodeId repeat_root = repeat.create_node(
            "Landscape", vcr::SceneNodeKind::folder);
        scatter.parent = repeat_root;
        std::vector<vcr::SceneNodeId> second;
        expect(
            vcr::scatter_model_instances(repeat, scatter, second, error) &&
                second.size() == first_transforms.size(),
            "scatter can be reproduced in a fresh scene");
        bool deterministic = second.size() == first_transforms.size();
        for (std::size_t index = 0U;
             deterministic && index < second.size(); ++index) {
            const auto node = repeat.node(second[index]);
            deterministic = node &&
                node->get().transform == first_transforms[index] &&
                node->get().asset_path == scatter.asset_path;
        }
        expect(
            deterministic,
            "scatter seed produces stable transforms independent of the STL RNG");

        const std::uint32_t size_before_invalid = repeat.size();
        scatter.minimum_scale = 2.0F;
        scatter.maximum_scale = 1.0F;
        expect(
            !vcr::scatter_model_instances(repeat, scatter, second, error) &&
                repeat.size() == size_before_invalid && second.empty(),
            "invalid scatter is rejected without a partial scene mutation");
    }

    {
        std::error_code filesystem_error;
        const std::filesystem::path temporary_root =
            std::filesystem::temp_directory_path(filesystem_error) /
            ("vcr_asset_browser_" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()));
        expect(
            !filesystem_error &&
                std::filesystem::create_directories(
                    temporary_root / "folder",
                    filesystem_error) &&
                !filesystem_error,
            "asset browser test directory is created");
        {
            std::ofstream image(
                temporary_root / "image.PNG",
                std::ios::binary);
            image.put('x');
        }
        {
            std::ofstream shader(
                temporary_root / "shader.vert",
                std::ios::binary);
            shader.put('x');
        }
        {
            std::ofstream nested(
                temporary_root / "folder" / "nested.glb",
                std::ios::binary);
            nested.put('x');
        }

        vcr::AssetBrowser browser;
        std::string error;
        expect(
            browser.initialize(temporary_root, error) &&
                error.empty() &&
                browser.entries().size() == 3U &&
                browser.entries().front().kind ==
                    vcr::AssetEntryKind::directory &&
                !browser.truncated(),
            "asset browser enumerates a bounded sorted project root");
        expect(
            !browser.navigate_to("..", error) &&
                browser.current_directory().empty() &&
                browser.entries().size() == 3U,
            "asset browser rejects root escape without changing state");
        expect(
            browser.navigate_to("folder", error) &&
                browser.current_directory() ==
                    std::filesystem::path("folder") &&
                browser.entries().size() == 1U &&
                browser.entries().front().kind ==
                    vcr::AssetEntryKind::model,
            "asset browser navigates into project assets");
        const std::size_t stable_entry_count =
            browser.entries().size();
        expect(
            !browser.navigate_to("missing", error) &&
                browser.current_directory() ==
                    std::filesystem::path("folder") &&
                browser.entries().size() == stable_entry_count,
            "failed asset navigation is transactional");
        expect(
            browser.navigate_up(error) &&
                browser.current_directory().empty(),
            "asset browser navigates to its root");

        vcr::SceneDocument startup_scene;
        const auto missing_startup = temporary_root / "local-game" / "scene.json";
        expect(vcr::load_startup_scene(missing_startup, startup_scene, error) &&
                   error.empty() && startup_scene.size() > 0U &&
                   !std::filesystem::exists(missing_startup),
               "missing local game starts a technical scene without writing game files");
        expect(!vcr::load_scene_file(missing_startup, startup_scene, error),
               "explicit Open still rejects a missing scene");

        vcr::SceneDocument source_scene;
        const vcr::SceneNodeId scene_root =
            source_scene.create_node(
                "Root", vcr::SceneNodeKind::folder);
        const vcr::SceneNodeId scene_cube =
            source_scene.create_node(
                "Cube", vcr::SceneNodeKind::cube, scene_root);
        const vcr::SceneNodeId scene_model =
            source_scene.create_node(
                "Imported tree",
                vcr::SceneNodeKind::model,
                scene_root);
        const vcr::SceneNodeId scene_point =
            source_scene.create_node(
                "Warm point", vcr::SceneNodeKind::point_light, scene_root);
        const vcr::SceneNodeId scene_spot =
            source_scene.create_node(
                "Cool spot", vcr::SceneNodeKind::spot_light, scene_root);
        const auto scene_ocean = source_scene.create_node(
            "Ocean", vcr::SceneNodeKind::ocean, scene_root);
        source_scene.node(scene_ocean)->get().transform.translation.y = -2.0F;
        const vcr::OceanSettings authored_ocean{
            .color = {0.02F, 0.3F, 0.4F},
            .wave_height = 2.5F,
            .wave_speed = 0.0F,
            .wave_length = 40.0F,
            .wave_direction_degrees = 270.0F,
            .depth = 8.0F,
            .transparency = 0.75F,
        };
        source_scene.node(scene_ocean)->get().ocean = authored_ocean;
        if (auto cube = source_scene.node(scene_cube)) {
            cube->get().transform.translation =
                glm::vec3(3.0F, 2.0F, -7.0F);
            cube->get().transform.rotation_degrees =
                glm::vec3(10.0F, 20.0F, 30.0F);
            cube->get().transform.scale =
                glm::vec3(2.0F, 0.5F, 4.0F);
            cube->get().material = vcr::PbrMaterial{
                .base_color =
                    glm::vec4(0.2F, 0.4F, 0.8F, 1.0F),
                .metallic = 0.75F,
                .roughness = 0.24F,
                .ambient_occlusion = 0.9F,
                .ior = 2.42F,
                .model = vcr::MaterialModel::unlit,
            };
            source_scene.mark_content_changed();
        }
        if (auto model = source_scene.node(scene_model)) {
            model->get().asset_path =
                "tree_stump_01_4k/"
                "tree_stump_01_4k.vcrasset.json";
            model->get().transform.translation =
                glm::vec3(-2.0F, 0.0F, -5.0F);
            source_scene.mark_content_changed();
        }
        if (auto point = source_scene.node(scene_point)) {
            point->get().light = vcr::PunctualLight{
                .color = {1.0F, 0.4F, 0.1F},
                .intensity_candela = 320.0F,
                .range = 14.0F,
            };
            point->get().transform.translation = {2.0F, 4.0F, -6.0F};
            source_scene.mark_content_changed();
        }
        if (auto spot = source_scene.node(scene_spot)) {
            spot->get().light = vcr::PunctualLight{
                .color = {0.1F, 0.4F, 1.0F},
                .intensity_candela = 640.0F,
                .range = 22.0F,
                .inner_cone_degrees = 12.0F,
                .outer_cone_degrees = 27.0F,
            };
            source_scene.mark_content_changed();
        }
        source_scene.tone_mapper = vcr::ToneMapper::gt7;
        source_scene.fog = vcr::FogSettings{
            .enabled = true,
            .color = {1.5F, 0.4F, 0.2F},
            .density = 0.015F,
            .height_falloff = 0.12F,
            .base_height = -4.0F,
            .start_distance = 18.0F,
            .maximum_distance = 640.0F,
            .maximum_opacity = 0.82F,
        };
        const std::filesystem::path scene_file =
            temporary_root / "roundtrip.scene.json";
        expect(
            vcr::save_scene_file(
                source_scene, scene_file, error),
            "scene JSON saves through a temporary file");
        vcr::SceneDocument loaded_scene;
        expect(
            vcr::load_scene_file(
                scene_file, loaded_scene, error) &&
                loaded_scene.size() == source_scene.size(),
            "scene JSON loads transactionally");
        std::vector<vcr::SceneNodeRecord> source_records;
        std::vector<vcr::SceneNodeRecord> loaded_records;
        source_scene.build_node_records(source_records);
        loaded_scene.build_node_records(loaded_records);
        expect(
            loaded_records.size() == 6U &&
                loaded_records[5].value.kind == vcr::SceneNodeKind::ocean &&
                loaded_records[5].value.transform.translation.y == -2.0F &&
                loaded_records[5].value.ocean == authored_ocean &&
                source_records.size() == loaded_records.size() &&
                loaded_records[1].parent_record_index == 0U &&
                loaded_records[1].value.name == "Cube" &&
                loaded_records[2].value.kind ==
                    vcr::SceneNodeKind::model &&
                loaded_records[2].value.asset_path ==
                    "tree_stump_01_4k/"
                    "tree_stump_01_4k.vcrasset.json" &&
                close_to(
                    loaded_records[1].value.transform.scale.z,
                    4.0,
                    0.0001) &&
                close_to(
                    loaded_records[1].value.material.metallic,
                    0.75,
                    0.0001) &&
                close_to(
                    loaded_records[1].value.material.roughness,
                    0.24,
                    0.0001) &&
                close_to(
                    loaded_records[1]
                        .value.material.ambient_occlusion,
                    0.9,
                    0.0001) &&
                loaded_records[1].value.material.ior == 2.42F &&
                loaded_records[1].value.material.model == vcr::MaterialModel::unlit &&
                loaded_records[3].value.kind == vcr::SceneNodeKind::point_light &&
                loaded_records[3].value.light.intensity_candela == 320.0F &&
                loaded_records[3].value.light.range == 14.0F &&
                loaded_records[4].value.kind == vcr::SceneNodeKind::spot_light &&
                loaded_records[4].value.light.inner_cone_degrees == 12.0F &&
                loaded_records[4].value.light.outer_cone_degrees == 27.0F &&
                loaded_scene.tone_mapper == vcr::ToneMapper::gt7 &&
                loaded_scene.fog && loaded_scene.fog->enabled &&
                loaded_scene.fog->color == std::array{1.5F, 0.4F, 0.2F} &&
                loaded_scene.fog->density == 0.015F &&
                loaded_scene.fog->maximum_distance == 640.0F,
            "scene JSON preserves hierarchy, materials, lights and rendering settings");

        nlohmann::json valid_material_scene;
        { std::ifstream input(scene_file); input >> valid_material_scene; }
        const auto invalid_material_file = temporary_root / "invalid-material.scene.json";
        for (const auto& invalid : {
                 nlohmann::json{{"wave_height", -0.1}},
                 nlohmann::json{{"wave_height", 11.0}},
                 nlohmann::json{{"wave_speed", -1.0}},
                 nlohmann::json{{"wave_length", 0.0}},
                 nlohmann::json{{"wave_length", 201.0}},
                 nlohmann::json{{"wave_direction_degrees", 361.0}},
                 nlohmann::json{{"depth", -1.0}},
                 nlohmann::json{{"depth", 1e100}},
                 nlohmann::json{{"transparency", 1.1}},
                 nlohmann::json{{"transparency", true}},
                 nlohmann::json{{"color", {1.0, -0.1, 0.0}}},
                 nlohmann::json{{"color", {1.0, 0.5}}}}) {
            auto invalid_scene = valid_material_scene;
            invalid_scene["nodes"][5]["ocean"].update(invalid);
            { std::ofstream output(invalid_material_file); output << invalid_scene; }
            const auto revision = loaded_scene.content_revision();
            expect(!vcr::load_scene_file(invalid_material_file, loaded_scene, error) &&
                       loaded_scene.content_revision() == revision,
                   "invalid ocean JSON is rejected without replacing the current scene");
        }
        {
            auto legacy = valid_material_scene;
            legacy["nodes"][5].erase("ocean");
            { std::ofstream output(invalid_material_file); output << legacy; }
            vcr::SceneDocument legacy_scene;
            expect(vcr::load_scene_file(invalid_material_file, legacy_scene, error),
                   "legacy ocean without settings still loads");
            std::vector<vcr::SceneNodeRecord> legacy_records;
            legacy_scene.build_node_records(legacy_records);
            expect(legacy_records.size() == 6 && legacy_records[5].value.ocean == vcr::OceanSettings{},
                   "legacy ocean receives safe defaults");
            legacy["nodes"][1]["ocean"] = nlohmann::json::object();
            { std::ofstream output(invalid_material_file); output << legacy; }
            expect(!vcr::load_scene_file(invalid_material_file, legacy_scene, error),
                   "ocean payload cannot be attached to a cube");
            auto invalid_save = source_scene;
            invalid_save.node(scene_ocean)->get().ocean.depth = std::numeric_limits<float>::quiet_NaN();
            expect(!vcr::save_scene_file(invalid_save, scene_file, error),
                   "non-finite ocean settings are rejected before replacing the saved scene");
        }
        for (const auto& invalid : {nlohmann::json{{"ior", 0}}, nlohmann::json{{"ior", .99}},
                nlohmann::json{{"ior", 1e100}}, nlohmann::json{{"ior", true}},
                nlohmann::json{{"ior", "1.5"}}, nlohmann::json{{"model", "sss"}},
                nlohmann::json{{"model", 1}}, nlohmann::json{{"model", nullptr}}}) {
            auto invalid_scene = valid_material_scene;
            invalid_scene["nodes"][1]["pbr"].update(invalid);
            { std::ofstream output(invalid_material_file); output << invalid_scene; }
            const auto revision = loaded_scene.content_revision();
            expect(!vcr::load_scene_file(invalid_material_file, loaded_scene, error) &&
                       loaded_scene.content_revision() == revision,
                   "invalid material JSON is rejected transactionally");
        }
        for (const auto& invalid : {
                 nlohmann::json{{"range", 0.0}},
                 nlohmann::json{{"intensity_candela", -1.0}},
                 nlohmann::json{{"color", nlohmann::json::array({1.0, 0.5})}}}) {
            auto invalid_scene = valid_material_scene;
            invalid_scene["nodes"][3]["light"].update(invalid);
            { std::ofstream output(invalid_material_file); output << invalid_scene; }
            const auto revision = loaded_scene.content_revision();
            expect(!vcr::load_scene_file(invalid_material_file, loaded_scene, error) &&
                       loaded_scene.content_revision() == revision,
                   "invalid point-light JSON is rejected transactionally");
        }
        {
            auto invalid_scene = valid_material_scene;
            invalid_scene["nodes"][4]["light"]["inner_cone_degrees"] = 30.0;
            invalid_scene["nodes"][4]["light"]["outer_cone_degrees"] = 20.0;
            { std::ofstream output(invalid_material_file); output << invalid_scene; }
            const auto revision = loaded_scene.content_revision();
            expect(!vcr::load_scene_file(invalid_material_file, loaded_scene, error) &&
                       loaded_scene.content_revision() == revision,
                   "inverted spot cone is rejected transactionally");
        }

        // A placeholder camera in a version-1 document must not change its old view.
        const auto camera_parent = source_scene.create_node("Camera rig", vcr::SceneNodeKind::folder);
        const auto camera_id = source_scene.create_node("Start", vcr::SceneNodeKind::camera, camera_parent);
        source_scene.node(camera_parent)->get().transform = {
            .translation = {10, 2, 3}, .rotation_degrees = {0, 90, 0}, .scale = {2, 2, 2}};
        source_scene.node(camera_id)->get().transform = {
            .translation = {0, 1, -3}, .rotation_degrees = {-12, 0, 0}};
        expect(!source_scene.startup_camera_pose() &&
                   vcr::scene_startup_camera(source_scene).position() == glm::vec3(0, 4, 12),
               "legacy placeholder camera keeps original startup pose");
        source_scene.startup_camera = camera_id;
        const auto authored_camera = vcr::scene_startup_camera(source_scene);
        expect(glm::length(authored_camera.position() - glm::vec3(4, 4, 3)) < .0001F &&
                   close_to(authored_camera.forward().x, -std::cos(glm::radians(12.0F)), .0001) &&
                   close_to(authored_camera.forward().y, -std::sin(glm::radians(12.0F)), .0001),
               "startup camera inherits rig translation scale and Euler orientation");
        expect(vcr::scene_startup_camera(source_scene, 12.0).position() == glm::vec3(0, 4, 12),
               "explicit default CLI Z still overrides authored pose");
        source_scene.lighting = vcr::LightingSettings{
            .cycle = false, .cascade_cache = true, .hour = 17.2,
            .shadow_depth = vcr::ShadowDepthFormat::d16};
        const auto startup_file = temporary_root / "startup.scene.json";
        expect(vcr::save_scene_file(source_scene, startup_file, error) &&
                   vcr::load_scene_file(startup_file, loaded_scene, error),
               "authored startup settings round trip");
        expect(loaded_scene.lighting && !loaded_scene.lighting->cycle &&
                   loaded_scene.lighting->cascade_cache &&
                   loaded_scene.lighting->shadow_depth == vcr::ShadowDepthFormat::d16 &&
                   close_to(loaded_scene.lighting->hour, 17.2, .0001) &&
                   glm::length(vcr::scene_startup_camera(loaded_scene).position() -
                               authored_camera.position()) < .0001F,
               "round trip preserves camera node reference and fixed evening");
        for (const auto depth : {vcr::ShadowDepthFormat::automatic, vcr::ShadowDepthFormat::d32,
                                 vcr::ShadowDepthFormat::d16}) {
            source_scene.lighting->shadow_depth = depth;
            expect(vcr::save_scene_file(source_scene, startup_file, error) &&
                       vcr::load_scene_file(startup_file, loaded_scene, error) &&
                       loaded_scene.lighting && loaded_scene.lighting->shadow_depth == depth,
                   "each shadow depth profile survives scene JSON round trip");
        }
        source_scene.lighting->shadow_depth = static_cast<vcr::ShadowDepthFormat>(255);
        expect(!vcr::save_scene_file(source_scene, startup_file, error),
               "scene writer rejects an invalid shadow depth enum");
        source_scene.lighting->shadow_depth = vcr::ShadowDepthFormat::d16;
        source_scene.tone_mapper = static_cast<vcr::ToneMapper>(255);
        expect(!vcr::save_scene_file(source_scene, startup_file, error),
               "scene writer rejects an invalid tone mapper enum");
        source_scene.tone_mapper = vcr::ToneMapper::gt7;
        const auto valid_fog = *source_scene.fog;
        source_scene.fog->density = std::numeric_limits<float>::quiet_NaN();
        expect(!vcr::save_scene_file(source_scene, startup_file, error),
               "scene writer rejects invalid fog settings");
        source_scene.fog = valid_fog;
        expect(source_scene.reparent(scene_cube, camera_parent) &&
                   vcr::save_scene_file(source_scene, startup_file, error) &&
                   vcr::load_scene_file(startup_file, loaded_scene, error) &&
                   glm::length(vcr::scene_startup_camera(loaded_scene).position() -
                               authored_camera.position()) < .0001F,
               "save remaps selected camera to serialized traversal index");
        const char *override_args[]{"test", "--time-of-day=14", "--gi", "off", "--camera-z=12"};
        const auto overrides = vcr::parse_cli_options(5, override_args);
        const auto merged = vcr::resolve_scene_lighting(overrides.options, source_scene.lighting);
        expect(overrides && overrides.options.camera_z_explicit && merged.hour == 14 &&
                   !merged.cycle && !merged.voxel_gi,
               "CLI overrides individual authored fields even at their default values");
        const char *legacy_args[]{"test", "--lighting=legacy"};
        const auto legacy_options = vcr::parse_cli_options(2, legacy_args);
        const auto legacy_lighting = vcr::resolve_scene_lighting(legacy_options.options, source_scene.lighting);
        expect(!legacy_lighting.dynamic && !legacy_lighting.voxel_gi && !legacy_lighting.cycle,
               "CLI legacy lighting disables dependent GI and clock");
        nlohmann::json startup_json;
        { std::ifstream input(startup_file); input >> startup_json; }
        expect(startup_json["lighting"]["shadow_depth"] == "d16",
               "scene JSON writes a stable string shadow depth profile");
        expect(startup_json["tone_mapper"] == "gt7" &&
                   startup_json["fog"]["enabled"] == true &&
                   startup_json["fog"]["color"].is_array() &&
                   startup_json["fog"]["color"].size() == 3U &&
                   close_to(startup_json["fog"]["color"][0].get<double>(), 1.5, 0.0001) &&
                   close_to(startup_json["fog"]["color"][1].get<double>(), 0.4, 0.0001) &&
                   close_to(startup_json["fog"]["color"][2].get<double>(), 0.2, 0.0001),
               "scene JSON writes stable tone-mapping and fog settings");
        const auto original_json = startup_json;
        const auto write_startup = [&] {
            std::ofstream output(startup_file);
            output << startup_json.dump();
        };
        for (const auto &bad : std::array<nlohmann::json, 4>{-1, 0, 999999, "camera"}) {
            startup_json = original_json;
            startup_json["startup_camera"] = bad;
            write_startup();
            const auto previous_size = loaded_scene.size();
            expect(!vcr::load_scene_file(startup_file, loaded_scene, error) &&
                       loaded_scene.size() == previous_size && loaded_scene.lighting->hour == 17.2,
                   "invalid startup camera rejects transaction without changing document");
        }
        for (const auto &bad : std::array<nlohmann::json, 15>{
                 nlohmann::json{{"hour", 24}}, nlohmann::json{{"cycle", "off"}},
                 nlohmann::json{{"triangle_budget", 2048.5}},
                 nlohmann::json{{"shadow_resolution", 3072}},
                 nlohmann::json{{"cascade_cache", "on"}},
                 nlohmann::json{{"shadow_depth", "d24"}},
                 nlohmann::json{{"shadow_depth", "D16"}},
                 nlohmann::json{{"shadow_depth", ""}},
                 nlohmann::json{{"shadow_depth", 16}},
                 nlohmann::json{{"shadow_depth", true}},
                 nlohmann::json{{"shadow_depth", nullptr}},
                 nlohmann::json{{"shadow_depth", nlohmann::json::array({"d16"})}},
                 nlohmann::json{{"shadow_depth", nlohmann::json{{"format", "d16"}}}},
                 nlohmann::json{{"dynamic", false}, {"voxel_gi", true}},
                 nlohmann::json{{"gi_extent", 1e100}}}) {
            startup_json = original_json;
            startup_json["lighting"] = bad;
            write_startup();
            expect(!vcr::load_scene_file(startup_file, loaded_scene, error) &&
                       loaded_scene.lighting &&
                       loaded_scene.lighting->shadow_depth == vcr::ShadowDepthFormat::d16,
                   "invalid lighting types ranges and combinations are rejected");
        }
        for (const auto& bad : std::array<nlohmann::json, 4>{
                 nlohmann::json{"GT7"}, nlohmann::json{""}, nlohmann::json{2},
                 nlohmann::json{nullptr}}) {
            startup_json = original_json;
            startup_json["tone_mapper"] = bad;
            write_startup();
            expect(!vcr::load_scene_file(startup_file, loaded_scene, error) &&
                       loaded_scene.tone_mapper == vcr::ToneMapper::gt7,
                   "invalid tone mapper rejects scene transactionally");
        }
        for (const auto& bad : std::array<nlohmann::json, 5>{
                 nlohmann::json{{"enabled", "yes"}},
                 nlohmann::json{{"density", -1.0}},
                 nlohmann::json{{"color", {1.0, 2.0}}},
                 nlohmann::json{{"color", {1.0, 2.0, 17.0}}},
                 nlohmann::json{{"start_distance", 500.0}, {"maximum_distance", 100.0}}}) {
            startup_json = original_json;
            startup_json["fog"] = bad;
            write_startup();
            expect(!vcr::load_scene_file(startup_file, loaded_scene, error) &&
                       loaded_scene.fog && loaded_scene.fog->density == 0.015F,
                   "invalid fog settings reject scene transactionally");
        }
        startup_json = original_json;
        startup_json["lighting"] = {{"hour", 17.2}, {"cycle", false}};
        write_startup();
        expect(vcr::load_scene_file(startup_file, loaded_scene, error) &&
                   loaded_scene.lighting->dynamic && loaded_scene.lighting->voxel_gi &&
                   loaded_scene.lighting->shadow_resolution == 2048 &&
                   loaded_scene.lighting->shadow_depth == vcr::ShadowDepthFormat::automatic &&
                   !loaded_scene.lighting->cascade_cache,
               "partial lighting gets current safe defaults");
        expect(source_scene.destroy_subtree(camera_parent), "delete authored camera rig");
        expect(vcr::save_scene_file(source_scene, startup_file, error) &&
                   vcr::load_scene_file(startup_file, loaded_scene, error) &&
                   !loaded_scene.startup_camera,
               "deleted camera selection is omitted rather than binding a reused slot");

        vcr::SceneDocument invalid_model_scene;
        static_cast<void>(invalid_model_scene.create_node(
            "Missing asset",
            vcr::SceneNodeKind::model));
        expect(
            !vcr::save_scene_file(
                invalid_model_scene,
                temporary_root / "invalid-model.scene.json",
                error),
            "scene save rejects a model without an asset path");

        const std::filesystem::path legacy_scene_file =
            temporary_root / "legacy.scene.json";
        {
            std::ofstream legacy(legacy_scene_file);
            legacy
                << "{\"format\":\"vulkan_cube_scene\","
                   "\"version\":1,\"nodes\":[{"
                   "\"name\":\"Legacy\",\"kind\":\"cube\","
                   "\"parent\":null,\"visible\":true,"
                   "\"expanded\":true,\"transform\":{"
                   "\"translation\":[0,0,0],"
                   "\"rotation_degrees\":[0,0,0],"
                   "\"scale\":[1,1,1]},"
                   "\"color\":[0.2,0.4,0.8,1]}]}";
        }
        vcr::SceneDocument legacy_scene;
        expect(
            vcr::load_scene_file(
                legacy_scene_file, legacy_scene, error),
            "legacy scene JSON without PBR data remains supported");
        std::vector<vcr::SceneNodeRecord> legacy_records;
        legacy_scene.build_node_records(legacy_records);
        expect(
            legacy_records.size() == 1U &&
                !legacy_scene.tone_mapper && !legacy_scene.fog &&
                close_to(
                    legacy_records[0].value.material.base_color.b,
                    0.8,
                    0.0001) &&
                close_to(
                    legacy_records[0].value.material.metallic,
                    0.0,
                    0.0001) &&
                close_to(
                    legacy_records[0].value.material.roughness,
                    0.65,
                    0.0001) &&
                close_to(
                    legacy_records[0]
                        .value.material.ambient_occlusion,
                    1.0,
                    0.0001),
            "legacy scene JSON receives safe PBR defaults");

        const std::filesystem::path invalid_scene_file =
            temporary_root / "invalid.scene.json";
        {
            std::ofstream invalid(invalid_scene_file);
            invalid
                << "{\"format\":\"vulkan_cube_scene\","
                   "\"version\":1,\"nodes\":[{"
                   "\"name\":\"Invalid\",\"kind\":\"cube\","
                   "\"parent\":1,\"visible\":true,"
                   "\"expanded\":true,\"transform\":{"
                   "\"translation\":[0,0,0],"
                   "\"rotation_degrees\":[0,0,0],"
                   "\"scale\":[1,1,1]},"
                   "\"color\":[1,1,1,1]}]}";
        }
        vcr::SceneDocument unchanged =
            vcr::SceneDocument::make_default();
        const std::uint32_t unchanged_size = unchanged.size();
        expect(!vcr::load_startup_scene(invalid_scene_file, unchanged, error) &&
                   !error.empty() && unchanged.size() == unchanged_size,
               "invalid local game is not silently replaced by the technical scene");
        expect(
            !vcr::load_scene_file(
                invalid_scene_file, unchanged, error) &&
                unchanged.size() == unchanged_size,
            "invalid scene load leaves the current document unchanged");

        const std::filesystem::path escaping_asset_scene_file =
            temporary_root / "escaping-asset.scene.json";
        {
            std::ofstream invalid(escaping_asset_scene_file);
            invalid
                << "{\"format\":\"vulkan_cube_scene\","
                   "\"version\":1,\"nodes\":[{"
                   "\"name\":\"Escaping\",\"kind\":\"model\","
                   "\"asset_path\":\"../outside.vcrasset.json\","
                   "\"parent\":null,\"visible\":true,"
                   "\"expanded\":true,\"transform\":{"
                   "\"translation\":[0,0,0],"
                   "\"rotation_degrees\":[0,0,0],"
                   "\"scale\":[1,1,1]},"
                   "\"color\":[1,1,1,1]}]}";
        }
        expect(
            !vcr::load_scene_file(
                escaping_asset_scene_file, unchanged, error) &&
                unchanged.size() == unchanged_size,
            "scene load rejects an asset-root escape transactionally");

        filesystem_error.clear();
        static_cast<void>(std::filesystem::remove_all(
            temporary_root, filesystem_error));
        expect(
            !filesystem_error,
            "asset browser test directory is removed");
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
