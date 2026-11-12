#include "audio_engine.hpp"
#include "cooked_audio_asset.hpp"
#include "input_actions.hpp"
#include "prefab_io.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "sound_cue_io.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(const bool condition, const char* const description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++g_failures;
    }
}

[[nodiscard]] bool close_to(
    const float a,
    const float b,
    const float eps = 1.0e-3F) noexcept {
    return std::abs(a - b) <= eps;
}

[[nodiscard]] bool write_test_wav_pcm16(
    const std::filesystem::path& path,
    const std::uint32_t sample_rate,
    const std::uint16_t channels,
    const std::uint32_t frame_count,
    const float frequency_hz,
    const float amplitude = 0.75F) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::uint32_t data_bytes =
        frame_count * static_cast<std::uint32_t>(channels) * 2U;
    const std::uint32_t riff_size = 36U + data_bytes;
    const std::uint32_t byte_rate =
        sample_rate * static_cast<std::uint32_t>(channels) * 2U;
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2U);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const auto write_u16 = [&](const std::uint16_t v) {
        const char bytes[2]{
            static_cast<char>(v & 0xFFU),
            static_cast<char>((v >> 8U) & 0xFFU),
        };
        out.write(bytes, 2);
    };
    const auto write_u32 = [&](const std::uint32_t v) {
        const char bytes[4]{
            static_cast<char>(v & 0xFFU),
            static_cast<char>((v >> 8U) & 0xFFU),
            static_cast<char>((v >> 16U) & 0xFFU),
            static_cast<char>((v >> 24U) & 0xFFU),
        };
        out.write(bytes, 4);
    };

    out.write("RIFF", 4);
    write_u32(riff_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(16U);
    write_u16(1U); // PCM
    write_u16(channels);
    write_u32(sample_rate);
    write_u32(byte_rate);
    write_u16(block_align);
    write_u16(16U);
    out.write("data", 4);
    write_u32(data_bytes);

    constexpr float two_pi = 2.0F * std::numbers::pi_v<float>;
    for (std::uint32_t f = 0U; f < frame_count; ++f) {
        const float phase =
            two_pi * frequency_hz * static_cast<float>(f) /
            static_cast<float>(sample_rate);
        const float s = std::clamp(std::sin(phase) * amplitude, -1.0F, 1.0F);
        const auto pcm = static_cast<std::int16_t>(std::lround(s * 32767.0F));
        for (std::uint16_t ch = 0U; ch < channels; ++ch) {
            write_u16(static_cast<std::uint16_t>(pcm));
        }
    }
    out.flush();
    return static_cast<bool>(out);
}

[[nodiscard]] float stereo_rms_channel(
    const std::span<const float> interleaved_stereo,
    const std::size_t channel) noexcept {
    if (interleaved_stereo.size() < 2U) {
        return 0.0F;
    }
    double sum_sq = 0.0;
    std::size_t frames = 0U;
    for (std::size_t i = channel; i < interleaved_stereo.size(); i += 2U) {
        const double v = static_cast<double>(interleaved_stereo[i]);
        sum_sq += v * v;
        ++frames;
    }
    return frames > 0U ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(frames))) : 0.0F;
}

void test_input_action_system(const std::filesystem::path& scratch_root) {
    vcr::InputActionSystem input;
    std::array<vcr::BindingConflict, 16U> conflicts{};
    expect(
        input.detect_conflicts(conflicts) == 0U,
        "default InputActionSystem bindings have zero intra-context conflicts");

    // Radial deadzone preserves partial analog stick tilt and clamps length <= 1.
    const glm::vec2 inside_dz =
        vcr::apply_radial_deadzone({0.10F, 0.05F}, 0.18F, 0.95F);
    expect(
        glm::length(inside_dz) == 0.0F,
        "radial deadzone zeroes stick deflection inside inner deadzone");
    const glm::vec2 half_tilt =
        vcr::apply_radial_deadzone({0.0F, 0.565F}, 0.18F, 0.95F);
    expect(
        close_to(half_tilt.y, 0.5F, 0.02F),
        "radial deadzone preserves smooth partial analog stick deflection");
    const glm::vec2 full_tilt =
        vcr::apply_radial_deadzone({0.9F, 0.9F}, 0.18F, 0.95F);
    expect(
        close_to(glm::length(full_tilt), 1.0F, 1.0e-3F),
        "radial deadzone normalizes diagonal stick deflection at 1.0");

    // Context isolation: Gameplay vs UI vs Cutscene.
    input.set_context(vcr::InputContext::gameplay);
    input.set_key_down(vcr::InputKey::space, true);
    input.set_key_down(vcr::InputKey::w, true);
    input.add_mouse_delta(12.5F, -4.0F);
    input.begin_frame(1.0F / 60.0F);
    expect(
        input.action_pressed(vcr::InputAction::jump) &&
            input.action_down(vcr::InputAction::jump) &&
            !input.action_pressed(vcr::InputAction::cutscene_skip),
        "Space triggers Jump in Gameplay context without firing Cutscene Skip");
    expect(
        close_to(input.move_axis().y, 1.0F) &&
            close_to(input.look_delta().x, 12.5F) &&
            close_to(input.look_delta().y, -4.0F),
        "Gameplay move_axis and mouse look_delta evaluate cleanly");

    // Switching to Cutscene context clears Gameplay latches and activates Cutscene bindings.
    input.set_key_down(vcr::InputKey::space, false);
    input.begin_frame(1.0F / 60.0F);
    input.set_context(vcr::InputContext::cutscene);
    input.set_key_down(vcr::InputKey::space, true);
    input.begin_frame(1.0F / 60.0F);
    expect(
        input.action_pressed(vcr::InputAction::cutscene_advance) &&
            !input.action_down(vcr::InputAction::jump) &&
            glm::length(input.move_axis()) == 0.0F,
        "Cutscene context routes Space to cutscene_advance and suppresses Gameplay move_axis");

    // UI context discrete navigation with synthetic gamepad stick & buttons.
    input.release_all();
    input.set_context(vcr::InputContext::ui);
    vcr::GamepadState pad{};
    pad.connected = true;
    pad.left_stick = {0.0F, 0.85F};
    pad.buttons_mask = vcr::gamepad_button_bit(vcr::GamepadButton::south);
    input.set_synthetic_gamepad(pad);
    input.begin_frame(1.0F / 60.0F);
    expect(
        input.action_pressed(vcr::InputAction::ui_confirm) &&
            close_to(input.ui_navigation_step().y, 1.0F),
        "UI context evaluates gamepad south button as ui_confirm and stick tilt as navigation step");
    input.begin_frame(1.0F / 60.0F);
    expect(
        input.ui_navigation_step().y == 0.0F,
        "UI stick navigation step latches until repeat cooldown expires");

    // Rumble motor timer decays deterministically.
    input.set_context(vcr::InputContext::gameplay);
    input.trigger_rumble(0.5F, 0.8F, 0.20F);
    expect(
        close_to(input.rumble_state().high_frequency, 0.8F),
        "trigger_rumble activates low/high frequency motor state");
    input.begin_frame(0.10F);
    expect(
        input.rumble_state().remaining_seconds > 0.05F,
        "rumble remains active before duration expires");
    input.begin_frame(0.15F);
    expect(
        input.rumble_state().low_frequency == 0.0F &&
            input.rumble_state().high_frequency == 0.0F,
        "rumble decays automatically to zero once duration elapses");

    // Rebinding, conflict detection, and JSON persistence round-trip.
    vcr::ActionBinding custom_interact =
        input.binding(vcr::InputAction::interact);
    custom_interact.primary_key = vcr::InputKey::space; // Conflicts with Jump!
    input.set_binding(vcr::InputAction::interact, custom_interact);
    expect(
        input.detect_conflicts(conflicts) >= 1U &&
            conflicts[0].shared_context == vcr::InputContext::gameplay &&
            conflicts[0].shared_key == vcr::InputKey::space,
        "detect_conflicts reports intra-context key collision between Jump and Interact");

    custom_interact.primary_key = vcr::InputKey::f;
    custom_interact.gamepad_button = vcr::GamepadButton::north;
    input.set_binding(vcr::InputAction::interact, custom_interact);
    expect(
        input.detect_conflicts(conflicts) == 0U,
        "resolving duplicate binding clears conflict list");

    vcr::GamepadConfig cfg = input.gamepad_config();
    cfg.stick_inner_deadzone = 0.22F;
    cfg.look_speed_pixels_per_sec = 900.0F;
    cfg.invert_look_y = true;
    input.set_gamepad_config(cfg);

    const std::filesystem::path json_path =
        scratch_root / "config" / "input_bindings.json";
    std::string error;
    expect(
        vcr::save_input_bindings_file(json_path, input, error),
        "save_input_bindings_file writes custom bindings JSON");

    vcr::InputActionSystem loaded;
    expect(
        vcr::load_input_bindings_file(json_path, loaded, error),
        "load_input_bindings_file restores custom bindings JSON");
    expect(
        loaded.binding(vcr::InputAction::interact).primary_key ==
                vcr::InputKey::f &&
            loaded.binding(vcr::InputAction::interact).gamepad_button ==
                vcr::GamepadButton::north &&
            close_to(loaded.gamepad_config().stick_inner_deadzone, 0.22F) &&
            loaded.gamepad_config().invert_look_y,
        "loaded InputActionSystem matches saved bindings and GamepadConfig");
}

void test_audio_assets_and_sound_cues(const std::filesystem::path& asset_root) {
    const std::filesystem::path wav_mono_24k =
        asset_root / "audio" / "tone_24k.wav";
    const std::filesystem::path wav_stereo_48k =
        asset_root / "audio" / "music_loop.wav";
    expect(
        write_test_wav_pcm16(wav_mono_24k, 24'000U, 1U, 12'000U, 440.0F),
        "write mono 24 kHz WAV fixture");
    expect(
        write_test_wav_pcm16(wav_stereo_48k, 48'000U, 2U, 24'000U, 330.0F),
        "write stereo 48 kHz WAV fixture");

    vcr::AudioSourceFormatInfo info{};
    std::string error;
    expect(
        vcr::inspect_audio_source_file(wav_mono_24k, info, error) &&
            info.sample_rate == 24'000U &&
            info.channels == 1U &&
            info.total_pcm_frames == 12'000U &&
            close_to(static_cast<float>(info.duration_seconds), 0.5F, 0.01F),
        "inspect_audio_source_file reads WAV header metadata without full decode");

    std::filesystem::path resolved{};
    std::filesystem::path rel{};
    std::string name{};
    bool stream_rec = false;
    expect(
        !vcr::resolve_sandboxed_audio_file(
            asset_root, "../outside.wav", resolved, rel, name, stream_rec, error),
        "resolve_sandboxed_audio_file rejects path traversal outside asset_root");

    vcr::CookedAudioAsset cooked{};
    expect(
        vcr::load_cooked_audio_asset(
            asset_root, "audio/tone_24k.wav", cooked, error, 48'000U) &&
            cooked.sample_rate == 48'000U &&
            cooked.channels == 1U &&
            cooked.frame_count >= 23'900U &&
            cooked.frame_count <= 24'100U,
        "load_cooked_audio_asset resamples 24 kHz mono WAV cleanly to 48 kHz");

    // Streaming reader loop wrap & non-loop EOF.
    vcr::AudioStreamReader stream;
    expect(
        stream.open(asset_root, "audio/music_loop.wav", 48'000U, error),
        "AudioStreamReader opens stereo WAV");
    std::vector<float> chunk(36'000U * 2U, 0.0F);
    bool reached_end = false;
    const std::size_t looped_frames =
        stream.read_stereo_frames(chunk, true, reached_end);
    expect(
        looped_frames == 36'000U && !reached_end,
        "AudioStreamReader seamlessly wraps past EOF when loop=true");
    stream.close();

    expect(
        stream.open(asset_root, "audio/music_loop.wav", 48'000U, error),
        "AudioStreamReader reopens stereo WAV for non-loop read");
    const std::size_t non_loop_frames =
        stream.read_stereo_frames(chunk, false, reached_end);
    expect(
        non_loop_frames == 24'000U && reached_end,
        "AudioStreamReader stops at exact frame count and sets reached_end when loop=false");

    // Sound Cue serialization & non-repeating variation selection.
    vcr::SoundCueAsset cue{};
    cue.name = "Footsteps";
    cue.bus = vcr::AudioBusId::sfx;
    cue.volume_min = 0.85F;
    cue.volume_max = 1.05F;
    cue.pitch_min = 0.92F;
    cue.pitch_max = 1.08F;
    cue.avoid_repeat = true;
    cue.entries = {
        {.asset_path = "audio/tone_24k.wav", .weight = 1.0F},
        {.asset_path = "audio/music_loop.wav", .weight = 1.0F},
        {.asset_path = "audio/tone_24k.wav", .weight = 1.0F},
    };
    expect(
        vcr::save_sound_cue_file(
            cue, asset_root, "audio/footsteps.vcrcue.json", error),
        "save_sound_cue_file writes .vcrcue.json");
    vcr::SoundCueAsset loaded_cue{};
    expect(
        vcr::load_sound_cue_file(
            asset_root, "audio/footsteps.vcrcue.json", loaded_cue, error) &&
            loaded_cue == cue,
        "load_sound_cue_file round-trips SoundCueAsset");

    std::uint64_t rng = 0x123456789ABCDEF0ULL;
    std::optional<std::size_t> prev{};
    bool immediate_repeat = false;
    bool range_ok = true;
    for (int i = 0; i < 200; ++i) {
        const auto pick =
            vcr::pick_sound_cue_variation(loaded_cue, rng, prev);
        if (!pick) {
            range_ok = false;
            break;
        }
        if (prev && pick->entry_index == *prev) {
            immediate_repeat = true;
        }
        if (pick->volume < 0.85F - 1.0e-4F ||
            pick->volume > 1.05F + 1.0e-4F ||
            pick->pitch < 0.92F - 1.0e-4F ||
            pick->pitch > 1.08F + 1.0e-4F) {
            range_ok = false;
        }
        prev = pick->entry_index;
    }
    expect(
        !immediate_repeat && range_ok,
        "pick_sound_cue_variation avoids immediate clip repeats and honors pitch/volume bounds");
}

void test_audio_engine_dsp_and_spatial(const std::filesystem::path& asset_root) {
    // Pure spatial math checks.
    expect(
        close_to(
            vcr::compute_distance_attenuation(
                vcr::AudioAttenuationModel::linear, 10.0F, 2.0F, 18.0F, 1.0F),
            0.5F),
        "linear distance attenuation equals 0.5 at midpoint");
    expect(
        close_to(
            vcr::compute_cone_attenuation(
                {0.0F, 0.0F, 0.0F},
                {0.0F, 0.0F, -1.0F},
                {0.0F, 0.0F, 5.0F},
                60.0F,
                120.0F,
                0.2F),
            0.2F),
        "directional cone applies outer_cone_gain behind emitter");
    const float approaching_pitch = vcr::compute_doppler_pitch(
        {0.0F, 0.0F, -10.0F},
        {0.0F, 0.0F, 30.0F},
        {0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F},
        1.0F);
    const float receding_pitch = vcr::compute_doppler_pitch(
        {0.0F, 0.0F, -10.0F},
        {0.0F, 0.0F, -30.0F},
        {0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F},
        1.0F);
    expect(
        approaching_pitch > 1.05F && receding_pitch < 0.95F,
        "Doppler pitch shifts upward when approaching and downward when receding");

    vcr::AudioEngine engine;
    std::string error;
    expect(
        engine.initialize(
            vcr::AudioEngineOptions{
                .asset_root = asset_root,
                .sample_rate = 48'000U,
                .enable_hardware_output = false,
            },
            error),
        "AudioEngine initializes deterministically in headless mode");

    // 1. Master brickwall limiter clamps hot multi-voice peaks <= 1.0.
    std::vector<float> hot_tone(4'800U, 0.95F);
    for (int v = 0; v < 4; ++v) {
        const auto h = engine.play_pcm_clip(
            "hot_clip",
            hot_tone,
            1U,
            vcr::AudioPlayParams{
                .bus = vcr::AudioBusId::sfx,
                .volume = 2.0F,
                .loop = true,
            },
            error);
        expect(static_cast<bool>(h), "play_pcm_clip spawns hot voice");
    }
    std::vector<float> mix_buf(2'048U * 2U, 0.0F);
    engine.render_frames_for_test(mix_buf);
    float max_abs = 0.0F;
    bool all_finite = true;
    for (const float s : mix_buf) {
        all_finite = all_finite && std::isfinite(s);
        max_abs = std::max(max_abs, std::abs(s));
    }
    expect(
        all_finite && max_abs > 0.85F && max_abs <= 1.0F &&
            engine.telemetry().master_limiter_gain < 0.85F,
        "Master brickwall limiter prevents digital clipping (> 8x input clamped <= 1.0)");
    engine.stop_all_audio();
    engine.render_frames_for_test(mix_buf);

    // 2. Play / Pause / Stop isolation between Gameplay buses and UI preview.
    expect(
        engine.play_preview("audio/tone_24k.wav", error) &&
            engine.is_preview_playing(),
        "play_preview starts UI preview playback");
    const auto sfx_voice = engine.play_asset(
        "audio/music_loop.wav",
        vcr::AudioPlayParams{
            .bus = vcr::AudioBusId::sfx,
            .volume = 0.8F,
            .loop = true,
        },
        error);
    expect(static_cast<bool>(sfx_voice), "play_asset starts SFX voice");

    engine.set_gameplay_paused(true);
    engine.set_bus_muted(vcr::AudioBusId::ui, true);
    engine.render_frames_for_test(mix_buf); // Settle 4 ms click-free UI mute ramp
    engine.render_frames_for_test(mix_buf);
    expect(
        stereo_rms_channel(mix_buf, 0U) < 1.0e-5F,
        "set_gameplay_paused(true) freezes SFX output");
    engine.set_bus_muted(vcr::AudioBusId::ui, false);
    engine.render_frames_for_test(mix_buf);
    expect(
        stereo_rms_channel(mix_buf, 0U) > 0.05F,
        "UI preview continues rendering while gameplay is paused");
    engine.set_gameplay_paused(false);
    engine.stop_gameplay_audio(0.0F);
    engine.render_frames_for_test(mix_buf);
    expect(
        engine.is_preview_playing() && !engine.is_voice_active(sfx_voice),
        "stop_gameplay_audio stops gameplay voices while preserving UI preview");
    engine.stop_preview();
    engine.stop_all_audio();
    engine.render_frames_for_test(mix_buf);

    // 3. Smooth dual-deck Music crossfade via streaming rings.
    expect(
        engine.crossfade_music("audio/music_loop.wav", 0.05F, true, error),
        "crossfade_music starts initial streaming deck");
    engine.pump_streams_sync_for_test();
    engine.render_frames_for_test(mix_buf);
    expect(
        stereo_rms_channel(mix_buf, 0U) > 0.05F,
        "streaming music deck produces non-zero audio");
    expect(
        engine.crossfade_music("audio/tone_24k.wav", 0.05F, true, error),
        "crossfade_music transitions to second track");
    engine.pump_streams_sync_for_test();
    for (int block = 0; block < 4; ++block) {
        engine.render_frames_for_test(mix_buf);
        engine.update(2048.0F / 48000.0F);
    }
    expect(
        engine.current_music_path() == "audio/tone_24k.wav" &&
            stereo_rms_channel(mix_buf, 0U) > 0.05F,
        "dual-deck crossfade completes cleanly without silence");
    engine.stop_all_audio();
    engine.render_frames_for_test(mix_buf);

    // 4. 3D equal-power stereo panning & raycast occlusion low-pass filter.
    engine.set_listener(vcr::AudioListenerState{
        .position = {0.0F, 0.0F, 0.0F},
        .forward = {0.0F, 0.0F, -1.0F},
        .up = {0.0F, 1.0F, 0.0F},
    });
    std::vector<float> hf_tone(9'600U, 0.0F);
    for (std::size_t i = 0U; i < hf_tone.size(); ++i) {
        hf_tone[i] = 0.7F * std::sin(
            2.0F * std::numbers::pi_v<float> * 9'000.0F *
            static_cast<float>(i) / 48'000.0F);
    }
    vcr::AudioPlayParams right_clear{
        .bus = vcr::AudioBusId::sfx,
        .volume = 1.0F,
        .loop = true,
        .spatial = true,
        .spatial_params = vcr::AudioSpatialParams{
            .position = {4.0F, 0.0F, 0.0F},
            .min_distance = 5.0F,
            .max_distance = 40.0F,
            .occlusion = 0.0F,
            .reverb_send = 0.0F,
        },
    };
    const auto clear_handle =
        engine.play_pcm_clip("hf_right_clear", hf_tone, 1U, right_clear, error);
    engine.render_frames_for_test(mix_buf);
    const float clear_left_rms = stereo_rms_channel(mix_buf, 0U);
    const float clear_right_rms = stereo_rms_channel(mix_buf, 1U);
    expect(
        clear_right_rms > clear_left_rms * 2.5F && clear_right_rms > 0.1F,
        "3D emitter on +X pans strongly to right stereo channel");

    vcr::AudioSpatialParams occluded_params = right_clear.spatial_params;
    occluded_params.occlusion = 1.0F;
    engine.update_voice(clear_handle, 1.0F, 1.0F, occluded_params);
    engine.render_frames_for_test(mix_buf);
    engine.render_frames_for_test(mix_buf);
    const float occluded_right_rms = stereo_rms_channel(mix_buf, 1U);
    expect(
        occluded_right_rms < clear_right_rms * 0.45F,
        "occlusion=1.0 low-pass + gain attenuation damps 9 kHz 3D emitter");
    engine.stop_all_audio();
    engine.render_frames_for_test(mix_buf);

    // 5. Reverb zone evaluation & Freeverb wet tail.
    const std::array<std::pair<glm::vec3, vcr::AudioReverbZoneComponent>, 1U>
        zones{{{
            glm::vec3{0.0F, 0.0F, 0.0F},
            vcr::AudioReverbZoneComponent{
                .inner_radius = 4.0F,
                .outer_radius = 12.0F,
                .wet_level = 0.65F,
                .decay_seconds = 2.5F,
                .damping = 0.3F,
                .enabled = true,
            },
        }}};
    const vcr::AudioReverbState rev_state =
        vcr::evaluate_reverb_zones_at_listener({2.0F, 0.0F, 0.0F}, zones);
    expect(
        rev_state.enabled && close_to(rev_state.wet_level, 0.65F),
        "evaluate_reverb_zones_at_listener returns full wet level inside inner_radius");
    engine.set_reverb_state(rev_state);

    std::vector<float> burst(1'024U, 0.0F);
    for (std::size_t i = 0U; i < burst.size(); ++i) {
        burst[i] = 0.8F * std::sin(
            2.0F * std::numbers::pi_v<float> * 660.0F *
            static_cast<float>(i) / 48'000.0F);
    }
    static_cast<void>(engine.play_pcm_clip(
        "reverb_burst",
        burst,
        1U,
        vcr::AudioPlayParams{
            .bus = vcr::AudioBusId::sfx,
            .volume = 1.0F,
            .loop = false,
            .spatial = true,
            .spatial_params = vcr::AudioSpatialParams{
                .position = {0.0F, 0.0F, -2.0F},
                .min_distance = 4.0F,
                .max_distance = 30.0F,
                .reverb_send = 1.0F,
            },
        },
        error));
    // Render first 2048 frames (consumes entire 1024-frame dry burst), then check tail!
    engine.render_frames_for_test(mix_buf);
    std::vector<float> tail_buf(2'048U * 2U, 0.0F);
    engine.render_frames_for_test(tail_buf);
    expect(
        stereo_rms_channel(tail_buf, 0U) > 1.0e-3F,
        "Freeverb reverb bus produces audible wet decay tail after dry burst ends");

    engine.shutdown();
}

void test_scene_and_prefab_audio_roundtrip(
    const std::filesystem::path& asset_root) {
    vcr::SceneDocument scene;
    const vcr::SceneNodeId emitter =
        scene.create_node("Campfire", vcr::SceneNodeKind::cube);
    const vcr::SceneNodeId cam_node =
        scene.create_node("MainCamera", vcr::SceneNodeKind::camera, emitter);
    const vcr::SceneNodeId cave_node =
        scene.create_node("CaveReverb", vcr::SceneNodeKind::folder, emitter);

    vcr::AudioSourceComponent src{};
    expect(
        src.set_path("audio/footsteps.vcrcue.json"),
        "AudioSourceComponent accepts .vcrcue.json path");
    src.bus = vcr::AudioBusId::sfx;
    src.attenuation = vcr::AudioAttenuationModel::exponential;
    src.volume = 1.25F;
    src.pitch = 0.95F;
    src.min_distance = 2.0F;
    src.max_distance = 28.0F;
    src.rolloff = 1.5F;
    src.doppler_factor = 0.8F;
    src.inner_cone_degrees = 45.0F;
    src.outer_cone_degrees = 110.0F;
    src.outer_cone_gain = 0.3F;
    src.loop = true;
    expect(
        scene.set_audio_source(emitter, src),
        "attach AudioSourceComponent to scene node");

    const vcr::AudioListenerComponent listener{
        .volume_scale = 0.9F,
        .doppler_scale = 1.2F,
        .enabled = true,
    };
    expect(
        scene.set_audio_listener(cam_node, listener),
        "attach AudioListenerComponent to camera node");

    const vcr::AudioReverbZoneComponent zone{
        .inner_radius = 5.0F,
        .outer_radius = 18.0F,
        .wet_level = 0.55F,
        .decay_seconds = 3.2F,
        .damping = 0.4F,
        .enabled = true,
    };
    expect(
        scene.set_reverb_zone(cave_node, zone),
        "attach AudioReverbZoneComponent to zone node");

    const std::filesystem::path scene_path =
        asset_root / "scenes" / "audio_test.vcrscene.json";
    std::string error;
    expect(
        vcr::save_scene_file(scene, scene_path, error),
        "save_scene_file writes audio components");

    vcr::SceneDocument loaded_scene;
    expect(
        vcr::load_scene_file(scene_path, loaded_scene, error),
        "load_scene_file reads audio components");
    expect(
        loaded_scene.audio_sources().size() == 1U &&
            loaded_scene.audio_listeners().size() == 1U &&
            loaded_scene.reverb_zones().size() == 1U &&
            loaded_scene.audio_sources().values()[0] == src &&
            loaded_scene.audio_listeners().values()[0] == listener &&
            loaded_scene.reverb_zones().values()[0] == zone,
        "Scene JSON round-trips AudioSource, AudioListener, and AudioReverbZone");

    // Prefab save + instantiate with override.
    expect(
        vcr::save_prefab_file(
            scene,
            emitter,
            asset_root,
            "prefabs/campfire_rig.vcrprefab.json",
            error,
            vcr::PrefabSaveOptions{}),
        "save_prefab_file serializes subtree with audio components");

    vcr::AudioSourceComponent overridden_src = src;
    overridden_src.volume = 0.42F;
    const std::array<vcr::PrefabNodeOverride, 1U> overrides{{
        vcr::PrefabNodeOverride{
            .node_index = 0U,
            .audio_source = overridden_src,
        },
    }};
    vcr::SceneDocument target_scene;
    vcr::PrefabInstantiateResult inst_res{};
    expect(
        vcr::instantiate_prefab_file(
            target_scene,
            asset_root,
            "prefabs/campfire_rig.vcrprefab.json",
            vcr::PrefabInstantiateOptions{.overrides = overrides},
            inst_res,
            error) &&
            inst_res.instantiated_nodes == 3U,
        "instantiate_prefab_file instantiates 3-node audio rig with override");
    const auto root_src = target_scene.audio_source(inst_res.root_id);
    expect(
        root_src && close_to(root_src->get().volume, 0.42F) &&
            target_scene.audio_listeners().size() == 1U &&
            target_scene.reverb_zones().size() == 1U,
        "instantiated prefab preserves child listener/reverb and applies root audio_source override");
}

} // namespace

int main() {
    std::error_code ec;
    const std::filesystem::path scratch_root =
        std::filesystem::temp_directory_path(ec) /
        "vcr_audio_input_tests_scratch";
    std::filesystem::remove_all(scratch_root, ec);
    std::filesystem::create_directories(scratch_root, ec);

    test_input_action_system(scratch_root);
    test_audio_assets_and_sound_cues(scratch_root);
    test_audio_engine_dsp_and_spatial(scratch_root);
    test_scene_and_prefab_audio_roundtrip(scratch_root);

    std::filesystem::remove_all(scratch_root, ec);
    if (g_failures != 0) {
        std::cerr << g_failures << " audio/input test(s) failed\n";
        return 1;
    }
    std::cout << "All audio & input tests passed\n";
    return 0;
}
