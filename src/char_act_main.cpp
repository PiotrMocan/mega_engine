#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "character_studio_shell.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace vcr {
namespace {

using Json = nlohmann::json;

void copy_to_buf(char* dst, std::size_t dst_size, std::string_view src) noexcept {
    if (dst == nullptr || dst_size == 0U) {
        return;
    }
    const std::size_t n = std::min(src.size(), dst_size - 1U);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

class CharActTool final : public ICharacterStudioTool {
public:
    [[nodiscard]] CharacterStudioToolMeta meta() const noexcept override {
        return CharacterStudioToolMeta{
            .tool_id = "char_act",
            .window_title = "VCR Char Act Studio (Motion, Facial Rig, Voice & Dialogue)",
            .short_badge = "03 // ACT & DLG",
            .cli_program_name = "vulkan_char_act",
            .exe_base_name = "vulkan_char_act",
        };
    }

    void on_package_loaded(const CharacterPackage& package) override {
        if (selected_node_idx_ >= package.dialogue.nodes.size()) {
            selected_node_idx_ = 0U;
        }
        sync_node_buffers(package);
        if (!package.identity.prompt.empty() && prompt_buf_[0] == '\0') {
            copy_to_buf(prompt_buf_.data(), prompt_buf_.size(), package.identity.prompt);
        }
    }

    void on_frame_tick(CharacterStudioShell& shell, float /*dt_seconds*/) override {
        if (!initialized_facial_) {
            const ActingMotionSpec& act = shell.package().acting;
            FacialPreviewParams& f = shell.preview_facial();
            f.smile = act.default_smile;
            f.brow_raise = act.default_brow_raise;
            f.frown = act.default_frown;
            if (!act.default_idle_clip.empty()) {
                shell.set_preview_clip(act.default_idle_clip);
            }
            initialized_facial_ = true;
        }
    }

    void handle_viewport_interaction(
        CharacterStudioShell& shell,
        const ViewportInteractionContext& ctx,
        CharacterToolFrameActions& actions) override {
        if (ctx.left_clicked && ctx.hit && ctx.hit->hit) {
            if (ctx.hit->position.y > shell.package().body.height_meters * 0.78F) {
                shell.focus_camera_face();
                actions.status_message = "Focused 12-bone facial rig.";
                actions.status_tone = EditorTone::info;
            }
        }
    }

    void draw_left_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const EditorUiKit& kit = shell.ui_kit();

        kit.section_header(EditorIcon::play, "12 Procedural Motion Clips");
        const auto& clips = shell.preview_clips();
        if (ImGui::BeginChild("##ClipList", ImVec2(-1.0F, 175.0F), ImGuiChildFlags_Borders)) {
            for (const AnimationClip& clip : clips) {
                const bool selected = (clip.name == shell.preview_clip());
                if (ImGui::Selectable(clip.name.c_str(), selected)) {
                    shell.set_preview_clip(clip.name);
                }
            }
        }
        ImGui::EndChild();

        bool paused = shell.preview_paused();
        if (ImGui::Button(paused ? "Play Motion" : "Pause Motion")) {
            shell.set_preview_paused(!paused);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Time")) {
            shell.set_preview_time(0.0F);
        }

        ImGui::Separator();
        kit.section_header(EditorIcon::renderer, "Temperament & Locomotion");
        static constexpr std::array<CharacterTemperament, 6> temps{
            CharacterTemperament::neutral, CharacterTemperament::warm,
            CharacterTemperament::stern, CharacterTemperament::cheerful,
            CharacterTemperament::guarded, CharacterTemperament::heroic};
        if (ImGui::BeginCombo("Temperament", to_token(pkg.acting.temperament).data())) {
            for (const auto t : temps) {
                if (ImGui::Selectable(to_token(t).data(), t == pkg.acting.temperament)) {
                    pkg.acting.temperament = t;
                    actions.mark_dirty = true;
                    actions.rebuild_preview = true;
                }
            }
            ImGui::EndCombo();
        }
        auto acting_slider = [&](const char* label, float* v, float mn, float mx) {
            if (ImGui::SliderFloat(label, v, mn, mx, "%.2f")) {
                actions.mark_dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                actions.rebuild_preview = true;
            }
        };
        acting_slider("Walk m/s", &pkg.acting.walk_speed_mps, 0.8F, 3.5F);
        acting_slider("Run m/s", &pkg.acting.run_speed_mps, 2.5F, 8.5F);
        acting_slider("Idle Breath", &pkg.acting.idle_breath_amplitude, 0.0F, 2.0F);
        acting_slider("Stride Energy", &pkg.acting.stride_energy, 0.2F, 2.0F);
        acting_slider("Arm Swing", &pkg.acting.arm_swing, 0.0F, 2.0F);

        ImGui::Separator();
        kit.section_header(EditorIcon::camera, "12-Bone Facial Rig & Visemes");
        FacialPreviewParams& fp = shell.preview_facial();
        ImGui::SliderFloat("Smile", &fp.smile, 0.0F, 1.0F);
        ImGui::SliderFloat("Frown", &fp.frown, 0.0F, 1.0F);
        ImGui::SliderFloat("Brow Raise", &fp.brow_raise, 0.0F, 1.0F);
        ImGui::SliderFloat("Eye Blink", &fp.eye_blink, 0.0F, 1.0F);
        ImGui::SliderFloat("Jaw Open", &fp.jaw_open, 0.0F, 1.0F);
        ImGui::SliderFloat("Lip Round", &fp.lip_round, 0.0F, 1.0F);
        ImGui::SliderFloat("Lip Wide", &fp.lip_wide, 0.0F, 1.0F);
        ImGui::SliderFloat("Eye Look X", &fp.eye_look.x, -1.0F, 1.0F);
        ImGui::SliderFloat("Eye Look Y", &fp.eye_look.y, -1.0F, 1.0F);
        if (ImGui::Button("Apply Defaults to Pkg")) {
            pkg.acting.default_smile = fp.smile;
            pkg.acting.default_brow_raise = fp.brow_raise;
            pkg.acting.default_frown = fp.frown;
            actions.mark_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Face")) {
            fp = FacialPreviewParams{};
        }
    }

    void draw_right_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        CharacterPackage& pkg = shell.package();
        const EditorUiKit& kit = shell.ui_kit();

        kit.section_header(EditorIcon::add, "AI Acting & Dialogue Writer");
        ImGui::InputTextMultiline(
            "##ActPrompt", prompt_buf_.data(), prompt_buf_.size(),
            ImVec2(-1.0F, 56.0F));
        if (kit.action_button(
                "GenActAi", EditorIcon::play,
                shell.ai_busy() ? "Writing..." : "Generate Acting & Dialogue (AI)",
                EditorButtonStyle::primary, !shell.ai_busy())) {
            const std::string p = prompt_buf_.data();
            const std::uint64_t seed = pkg.identity.seed;
            shell.enqueue_ai_generation("Acting, Voice & Dialogue",
                [p, seed](const OllamaBridgeConfig& cfg,
                          CharacterPackage& target,
                          CharacterAiGenerationResult& res) {
                    return ai_generate_acting_spec(p, seed, cfg, target, res);
                });
        }

        ImGui::Separator();
        kit.section_header(EditorIcon::renderer, "Formant Voice Synthesizer");
        if (ImGui::SliderFloat("Pitch (Hz)", &pkg.voice.pitch_hz, 70.0F, 260.0F, "%.0f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::SliderFloat("Speech Rate", &pkg.voice.speech_rate, 0.55F, 1.75F, "%.2f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::SliderFloat("Cadence", &pkg.voice.cadence_variance, 0.0F, 1.0F, "%.2f")) {
            actions.mark_dirty = true;
        }
        if (ImGui::SliderFloat("Warmth", &pkg.voice.warmth, 0.0F, 1.0F, "%.2f")) {
            actions.mark_dirty = true;
        }

        ImGui::Checkbox("Audition Russian (RU) Line", &preview_russian_);

        ImGui::Separator();
        kit.section_header(EditorIcon::environment, "Dialogue Graph (EN / RU)");
        if (ImGui::Button("+ Add Node")) {
            DialogueNodeSpec n{};
            n.id = "node_" + std::to_string(pkg.dialogue.nodes.size() + 1U);
            n.speaker = pkg.identity.display_name;
            n.text = "Keep moving, traveler.";
            n.text_ru = "Двигайся дальше, путник.";
            n.voice_clip_id = n.id;
            pkg.dialogue.nodes.push_back(std::move(n));
            selected_node_idx_ = pkg.dialogue.nodes.size() - 1U;
            sync_node_buffers(pkg);
            actions.mark_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("- Remove Node") && pkg.dialogue.nodes.size() > 1U &&
            selected_node_idx_ < pkg.dialogue.nodes.size()) {
            pkg.dialogue.nodes.erase(
                pkg.dialogue.nodes.begin() + static_cast<std::ptrdiff_t>(selected_node_idx_));
            if (selected_node_idx_ >= pkg.dialogue.nodes.size()) {
                selected_node_idx_ = pkg.dialogue.nodes.size() - 1U;
            }
            sync_node_buffers(pkg);
            actions.mark_dirty = true;
        }

        if (ImGui::BeginChild("##DlgNodes", ImVec2(-1.0F, 92.0F), ImGuiChildFlags_Borders)) {
            for (std::size_t ni = 0U; ni < pkg.dialogue.nodes.size(); ++ni) {
                const bool sel = (ni == selected_node_idx_);
                const std::string label = pkg.dialogue.nodes[ni].id + " [" +
                    pkg.dialogue.nodes[ni].speaker + "]";
                if (ImGui::Selectable(label.c_str(), sel)) {
                    selected_node_idx_ = ni;
                    sync_node_buffers(pkg);
                }
            }
        }
        ImGui::EndChild();

        if (selected_node_idx_ < pkg.dialogue.nodes.size()) {
            DialogueNodeSpec& node = pkg.dialogue.nodes[selected_node_idx_];
            if (ImGui::InputText("Node ID", node_id_buf_.data(), node_id_buf_.size())) {
                node.id = sanitize_character_slug(node_id_buf_.data());
                node.voice_clip_id = node.id;
                actions.mark_dirty = true;
            }
            if (ImGui::InputText("Speaker", speaker_buf_.data(), speaker_buf_.size())) {
                node.speaker = speaker_buf_.data();
                actions.mark_dirty = true;
            }
            if (ImGui::InputTextMultiline("Text (EN)", text_en_buf_.data(), text_en_buf_.size(),
                    ImVec2(-1.0F, 52.0F))) {
                node.text = text_en_buf_.data();
                actions.mark_dirty = true;
            }
            if (ImGui::InputTextMultiline("Text (RU)", text_ru_buf_.data(), text_ru_buf_.size(),
                    ImVec2(-1.0F, 52.0F))) {
                node.text_ru = text_ru_buf_.data();
                actions.mark_dirty = true;
            }

            if (kit.action_button(
                    "SpeakLine", EditorIcon::play, "Synthesize, Speak & Lip-Sync",
                    EditorButtonStyle::primary)) {
                const std::string& line = (preview_russian_ && !node.text_ru.empty())
                    ? node.text_ru
                    : node.text;
                SynthesizedSpeechClip speech = synthesize_dialogue_speech(
                    node.id, line, pkg.voice, shell.preview_mesh().skeleton, pkg.identity.seed);
                const float dur = speech.duration_seconds;
                const std::size_t vc = speech.visemes.size();
                shell.set_preview_clip("talk");
                shell.preview_facial().smile = node.smile;
                shell.preview_facial().brow_raise = node.brow_raise;
                shell.preview_facial().frown = node.frown;
                shell.start_speech_preview(std::move(speech));
                actions.status_message = "Speaking '" + node.id + "' (" +
                    std::to_string(dur) + "s, " + std::to_string(vc) + " visemes)";
                actions.status_tone = EditorTone::success;
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop Audio")) {
                shell.stop_speech_preview();
            }
        }
    }

    void draw_bottom_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) override {
        const CharacterPackage& pkg = shell.package();
        ImGui::Text(
            "Clip: %s | t=%.2fs | Facial Bones: 12 / Total Rig: %zu | Speech: %s",
            shell.preview_clip().c_str(),
            shell.preview_time(),
            shell.preview_mesh().skeleton.bones.size(),
            shell.speech_preview_active() ? "SPEAKING (Viseme Sync Active)" : "Idle");
        ImGui::SameLine();
        if (ImGui::SmallButton("Export Selected .wav") &&
            selected_node_idx_ < pkg.dialogue.nodes.size()) {
            const DialogueNodeSpec& node = pkg.dialogue.nodes[selected_node_idx_];
            const std::string& line = (preview_russian_ && !node.text_ru.empty())
                ? node.text_ru
                : node.text;
            const SynthesizedSpeechClip speech = synthesize_dialogue_speech(
                node.id, line, pkg.voice, shell.preview_mesh().skeleton, pkg.identity.seed);
            const std::filesystem::path out_wav = shell.project_directory() / "assets" / "audio" /
                (pkg.identity.slug + "_" + node.id + ".wav");
            std::string err;
            if (write_pcm_wav_file(out_wav, speech.mono_pcm, speech.sample_rate, err)) {
                actions.status_message = "Exported " + out_wav.generic_string();
                actions.status_tone = EditorTone::success;
            } else {
                actions.status_message = "WAV export failed: " + err;
                actions.status_tone = EditorTone::danger;
            }
        }
    }

    int run_cli(const CharacterStudioCliArgs& args) override {
        const std::filesystem::path pkg_path = args.character_path.empty()
            ? default_character_package_path(args.project_directory, args.character_slug)
            : args.character_path;
        CharacterPackage pkg{};
        std::string err;
        if (!load_or_create_character_package(pkg_path, args.character_slug, pkg, err)) {
            std::cerr << "vulkan_char_act: failed to load package: " << err << "\n";
            return 1;
        }

        if (const auto prompt = args.find_option("prompt")) {
            CharacterAiGenerationResult ai_res{};
            if (!ai_generate_acting_spec(*prompt, pkg.identity.seed, args.ollama, pkg, ai_res)) {
                std::cerr << "vulkan_char_act: AI generation error: " << ai_res.diagnostic << "\n";
                return 1;
            }
            std::cout << "[ai] " << ai_res.summary << "\n";
        }
        if (const auto v = args.find_option("temperament")) {
            if (const auto t = parse_character_temperament(*v)) {
                pkg.acting.temperament = *t;
            }
        }
        if (const auto v = args.find_option("pitch")) {
            try { pkg.voice.pitch_hz = std::stof(std::string(*v)); } catch (...) {}
        }
        if (const auto v = args.find_option("rate")) {
            try { pkg.voice.speech_rate = std::stof(std::string(*v)); } catch (...) {}
        }
        if (const auto v = args.find_option("walk-speed")) {
            try { pkg.acting.walk_speed_mps = std::stof(std::string(*v)); } catch (...) {}
        }
        if (const auto v = args.find_option("run-speed")) {
            try { pkg.acting.run_speed_mps = std::stof(std::string(*v)); } catch (...) {}
        }
        if (const auto v = args.find_option("add-node")) {
            // Format: id:speaker:text_en[:text_ru]
            std::string_view sv = *v;
            const std::size_t p1 = sv.find(':');
            const std::size_t p2 = p1 != std::string_view::npos ? sv.find(':', p1 + 1U) : std::string_view::npos;
            if (p1 != std::string_view::npos && p2 != std::string_view::npos) {
                const std::size_t p3 = sv.find(':', p2 + 1U);
                DialogueNodeSpec node{};
                node.id = sanitize_character_slug(sv.substr(0U, p1));
                node.speaker = std::string(sv.substr(p1 + 1U, p2 - p1 - 1U));
                if (p3 != std::string_view::npos) {
                    node.text = std::string(sv.substr(p2 + 1U, p3 - p2 - 1U));
                    node.text_ru = std::string(sv.substr(p3 + 1U));
                } else {
                    node.text = std::string(sv.substr(p2 + 1U));
                }
                node.voice_clip_id = node.id;
                pkg.dialogue.nodes.push_back(std::move(node));
            }
        }

        sanitize_character_package(pkg);
        if (!save_character_package_file(pkg_path, pkg, err)) {
            std::cerr << "vulkan_char_act: save failed: " << err << "\n";
            return 1;
        }
        std::cout << "Saved acting/dialogue: " << pkg_path.generic_string()
                  << " (" << pkg.dialogue.nodes.size() << " nodes)\n";

        if (const auto wav_out = args.find_option("export-wav")) {
            const CookedSkeletonAsset skel = build_character_skeleton(pkg);
            const std::string& text = pkg.dialogue.nodes.empty()
                ? std::string("Ready.")
                : pkg.dialogue.nodes.front().text;
            const SynthesizedSpeechClip clip = synthesize_dialogue_speech(
                "preview", text, pkg.voice, skel, pkg.identity.seed);
            if (!write_pcm_wav_file(std::filesystem::path(*wav_out), clip.mono_pcm, clip.sample_rate, err)) {
                std::cerr << "vulkan_char_act: wav export failed: " << err << "\n";
                return 1;
            }
            std::cout << "Synthesized WAV: " << *wav_out
                      << " (" << clip.duration_seconds << "s, "
                      << clip.visemes.size() << " visemes)\n";
        }

        if (args.cook_after_cli) {
            CharacterCookReport report{};
            if (!cook_character_package_to_project(
                    pkg, args.project_directory, CharacterCookOptions{}, report, err)) {
                std::cerr << "vulkan_char_act: cook failed: " << err << "\n";
                return 1;
            }
            std::cout << "Cooked " << report.cooked_file_count << " assets ("
                      << report.audio_assets_relative.size() << " voices, "
                      << report.animation_clips_relative.size() << " clips)\n";
        }
        return 0;
    }

    void print_cli_help() const override {
        std::cout
            << "vulkan_char_act - VCR Single-Purpose Motion, Facial Rig, Voice & Dialogue Studio\n"
            << "Usage:\n"
            << "  vulkan_char_act [options]\n\n"
            << "Options:\n"
            << "  --project <dir>         Target game project directory\n"
            << "  --character <path|slug> .vcrchar.json path or character slug\n"
            << "  --cli                   Run headless CLI mode\n"
            << "  --prompt <text>         Generate acting, voice & EN/RU dialogue via AI\n"
            << "  --temperament <mode>    neutral|warm|stern|cheerful|guarded|heroic\n"
            << "  --pitch <hz>            Formant voice fundamental frequency\n"
            << "  --rate <mult>           Speech speed multiplier\n"
            << "  --add-node <id:sp:en[:ru]> Append bilingual dialogue node\n"
            << "  --export-wav <path>     Synthesize first node to PCM WAV\n"
            << "  --cook                  Cook character into <project>/assets/\n"
            << "  --smoke-frames <N>      Run N Vulkan GUI frames and exit\n";
    }

    bool handle_mcp_custom_method(
        std::string_view method,
        std::string_view params_json,
        CharacterPackage& package,
        const OllamaBridgeConfig& ollama,
        std::string& out_result_json,
        bool& out_rebuild_preview,
        std::string& out_error) override {
        if (method == "ai_generate_acting") {
            Json p = Json::object();
            try {
                if (!params_json.empty()) p = Json::parse(params_json);
            } catch (...) {}
            const std::string prompt = p.value("prompt", std::string{"stoic veteran"});
            CharacterAiGenerationResult res{};
            if (!ai_generate_acting_spec(prompt, package.identity.seed, ollama, package, res)) {
                out_error = res.diagnostic;
                return false;
            }
            out_rebuild_preview = true;
            out_result_json = Json{
                {"used_ollama", res.used_ollama},
                {"summary", res.summary},
                {"dialogue_nodes", package.dialogue.nodes.size()},
            }.dump();
            return true;
        }
        out_error = "Unknown vulkan_char_act MCP method: " + std::string(method);
        return false;
    }

private:
    void sync_node_buffers(const CharacterPackage& pkg) noexcept {
        if (pkg.dialogue.nodes.empty() || selected_node_idx_ >= pkg.dialogue.nodes.size()) {
            node_id_buf_[0] = '\0';
            speaker_buf_[0] = '\0';
            text_en_buf_[0] = '\0';
            text_ru_buf_[0] = '\0';
            return;
        }
        const DialogueNodeSpec& n = pkg.dialogue.nodes[selected_node_idx_];
        copy_to_buf(node_id_buf_.data(), node_id_buf_.size(), n.id);
        copy_to_buf(speaker_buf_.data(), speaker_buf_.size(), n.speaker);
        copy_to_buf(text_en_buf_.data(), text_en_buf_.size(), n.text);
        copy_to_buf(text_ru_buf_.data(), text_ru_buf_.size(), n.text_ru);
    }

    bool initialized_facial_ = false;
    bool preview_russian_ = false;
    std::size_t selected_node_idx_ = 0U;
    std::array<char, 512> prompt_buf_{"calm tactical commander, warm low voice, bilingual greeting"};
    std::array<char, 64> node_id_buf_{};
    std::array<char, 96> speaker_buf_{};
    std::array<char, 512> text_en_buf_{};
    std::array<char, 512> text_ru_buf_{};
};

} // namespace
} // namespace vcr

int main(int argc, char** argv) {
    try {
        vcr::CharActTool tool;
        vcr::CharacterStudioShell shell;
        return shell.run(argc, argv, tool);
    } catch (const std::exception& ex) {
        std::cerr << "vulkan_char_act fatal error: " << ex.what() << '\n';
        return 1;
    }
}
