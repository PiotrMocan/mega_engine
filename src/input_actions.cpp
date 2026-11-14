#include "input_actions.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::string_view bindings_schema = "vcr.input_bindings";
constexpr std::uint32_t bindings_schema_version = 1U;

struct KeyTokenEntry {
    InputKey key;
    std::string_view token;
};

constexpr std::array<KeyTokenEntry, input_key_count> key_tokens{{
    {InputKey::none, "none"},
    {InputKey::a, "a"},
    {InputKey::b, "b"},
    {InputKey::c, "c"},
    {InputKey::d, "d"},
    {InputKey::e, "e"},
    {InputKey::f, "f"},
    {InputKey::g, "g"},
    {InputKey::h, "h"},
    {InputKey::i, "i"},
    {InputKey::j, "j"},
    {InputKey::k, "k"},
    {InputKey::l, "l"},
    {InputKey::m, "m"},
    {InputKey::n, "n"},
    {InputKey::o, "o"},
    {InputKey::p, "p"},
    {InputKey::q, "q"},
    {InputKey::r, "r"},
    {InputKey::s, "s"},
    {InputKey::t, "t"},
    {InputKey::u, "u"},
    {InputKey::v, "v"},
    {InputKey::w, "w"},
    {InputKey::x, "x"},
    {InputKey::y, "y"},
    {InputKey::z, "z"},
    {InputKey::digit_0, "0"},
    {InputKey::digit_1, "1"},
    {InputKey::digit_2, "2"},
    {InputKey::digit_3, "3"},
    {InputKey::digit_4, "4"},
    {InputKey::digit_5, "5"},
    {InputKey::digit_6, "6"},
    {InputKey::digit_7, "7"},
    {InputKey::digit_8, "8"},
    {InputKey::digit_9, "9"},
    {InputKey::space, "space"},
    {InputKey::enter, "enter"},
    {InputKey::escape, "escape"},
    {InputKey::tab, "tab"},
    {InputKey::backspace, "backspace"},
    {InputKey::left_shift, "left_shift"},
    {InputKey::right_shift, "right_shift"},
    {InputKey::left_ctrl, "left_ctrl"},
    {InputKey::right_ctrl, "right_ctrl"},
    {InputKey::left_alt, "left_alt"},
    {InputKey::right_alt, "right_alt"},
    {InputKey::up, "up"},
    {InputKey::down, "down"},
    {InputKey::left, "left"},
    {InputKey::right, "right"},
    {InputKey::f1, "f1"},
    {InputKey::f2, "f2"},
    {InputKey::f3, "f3"},
    {InputKey::f4, "f4"},
    {InputKey::f5, "f5"},
    {InputKey::f6, "f6"},
    {InputKey::f7, "f7"},
    {InputKey::f8, "f8"},
    {InputKey::f9, "f9"},
    {InputKey::f10, "f10"},
    {InputKey::f11, "f11"},
    {InputKey::f12, "f12"},
    {InputKey::mouse_left, "mouse_left"},
    {InputKey::mouse_right, "mouse_right"},
    {InputKey::mouse_middle, "mouse_middle"},
    {InputKey::mouse_x1, "mouse_x1"},
    {InputKey::mouse_x2, "mouse_x2"},
}};

struct GamepadTokenEntry {
    GamepadButton button;
    std::string_view token;
};

constexpr std::array<GamepadTokenEntry, gamepad_button_count> gamepad_tokens{{
    {GamepadButton::none, "none"},
    {GamepadButton::south, "south"},
    {GamepadButton::east, "east"},
    {GamepadButton::west, "west"},
    {GamepadButton::north, "north"},
    {GamepadButton::left_shoulder, "left_shoulder"},
    {GamepadButton::right_shoulder, "right_shoulder"},
    {GamepadButton::back, "back"},
    {GamepadButton::start, "start"},
    {GamepadButton::left_stick, "left_stick"},
    {GamepadButton::right_stick, "right_stick"},
    {GamepadButton::dpad_up, "dpad_up"},
    {GamepadButton::dpad_down, "dpad_down"},
    {GamepadButton::dpad_left, "dpad_left"},
    {GamepadButton::dpad_right, "dpad_right"},
    {GamepadButton::left_trigger, "left_trigger"},
    {GamepadButton::right_trigger, "right_trigger"},
}};

struct ActionMetadata {
    InputAction action;
    std::string_view token;
    std::string_view display_name;
    ActionBinding default_binding;
};

constexpr std::array<ActionMetadata, input_action_count> action_table{{
    {InputAction::move_forward,
     "move_forward",
     "Move Forward",
     {InputKey::w, InputKey::up, GamepadButton::dpad_up}},
    {InputAction::move_backward,
     "move_backward",
     "Move Backward",
     {InputKey::s, InputKey::down, GamepadButton::dpad_down}},
    {InputAction::move_left,
     "move_left",
     "Move Left",
     {InputKey::a, InputKey::left, GamepadButton::dpad_left}},
    {InputAction::move_right,
     "move_right",
     "Move Right",
     {InputKey::d, InputKey::right, GamepadButton::dpad_right}},
    {InputAction::jump,
     "jump",
     "Jump",
     {InputKey::space, InputKey::none, GamepadButton::south}},
    {InputAction::run,
     "run",
     "Run / Sprint",
     {InputKey::left_shift, InputKey::right_shift, GamepadButton::left_stick}},
    {InputAction::interact,
     "interact",
     "Interact",
     {InputKey::e, InputKey::f, GamepadButton::west}},
    {InputAction::pause,
     "pause",
     "Pause",
     {InputKey::escape, InputKey::p, GamepadButton::start}},
    {InputAction::secondary_up,
     "secondary_up",
     "Secondary Forward / Positive",
     {InputKey::w, InputKey::up, GamepadButton::right_trigger}},
    {InputAction::secondary_down,
     "secondary_down",
     "Secondary Backward / Negative",
     {InputKey::s, InputKey::down, GamepadButton::left_trigger}},
    {InputAction::secondary_left,
     "secondary_left",
     "Secondary Left",
     {InputKey::a, InputKey::left, GamepadButton::none}},
    {InputAction::secondary_right,
     "secondary_right",
     "Secondary Right",
     {InputKey::d, InputKey::right, GamepadButton::none}},
    {InputAction::secondary_action,
     "secondary_action",
     "Secondary Action",
     {InputKey::space, InputKey::none, GamepadButton::east}},
    {InputAction::recover,
     "recover",
     "Recover / Reset Pose",
     {InputKey::r, InputKey::none, GamepadButton::right_shoulder}},
    {InputAction::quick_reset,
     "quick_reset",
     "Quick Reset",
     {InputKey::f5, InputKey::none, GamepadButton::back}},
    {InputAction::ui_up,
     "ui_up",
     "UI Up",
     {InputKey::up, InputKey::w, GamepadButton::dpad_up}},
    {InputAction::ui_down,
     "ui_down",
     "UI Down",
     {InputKey::down, InputKey::s, GamepadButton::dpad_down}},
    {InputAction::ui_left,
     "ui_left",
     "UI Left",
     {InputKey::left, InputKey::a, GamepadButton::dpad_left}},
    {InputAction::ui_right,
     "ui_right",
     "UI Right",
     {InputKey::right, InputKey::d, GamepadButton::dpad_right}},
    {InputAction::ui_confirm,
     "ui_confirm",
     "UI Confirm",
     {InputKey::enter, InputKey::space, GamepadButton::south}},
    {InputAction::ui_cancel,
     "ui_cancel",
     "UI Cancel",
     {InputKey::escape, InputKey::backspace, GamepadButton::east}},
    {InputAction::cutscene_skip,
     "cutscene_skip",
     "Skip Cutscene",
     {InputKey::escape, InputKey::backspace, GamepadButton::east}},
    {InputAction::cutscene_advance,
     "cutscene_advance",
     "Advance Dialogue",
     {InputKey::space, InputKey::enter, GamepadButton::south}},
}};

[[nodiscard]] GamepadConfig sanitize_gamepad_config(
    GamepadConfig cfg) noexcept {
    if (!std::isfinite(cfg.stick_inner_deadzone)) {
        cfg.stick_inner_deadzone = 0.18F;
    }
    cfg.stick_inner_deadzone =
        std::clamp(cfg.stick_inner_deadzone, 0.0F, 0.85F);
    if (!std::isfinite(cfg.stick_outer_deadzone)) {
        cfg.stick_outer_deadzone = 0.95F;
    }
    cfg.stick_outer_deadzone = std::clamp(
        cfg.stick_outer_deadzone,
        cfg.stick_inner_deadzone + 0.05F,
        1.0F);
    if (!std::isfinite(cfg.trigger_threshold)) {
        cfg.trigger_threshold = 0.35F;
    }
    cfg.trigger_threshold = std::clamp(cfg.trigger_threshold, 0.05F, 0.95F);
    if (!std::isfinite(cfg.look_speed_pixels_per_sec)) {
        cfg.look_speed_pixels_per_sec = 720.0F;
    }
    cfg.look_speed_pixels_per_sec =
        std::clamp(cfg.look_speed_pixels_per_sec, 10.0F, 5000.0F);
    return cfg;
}

[[nodiscard]] bool contexts_overlap(
    const InputAction a,
    const InputAction b,
    InputContext& out_shared) noexcept {
    constexpr std::array<InputContext, 4U> all_contexts{
        InputContext::gameplay,
        InputContext::ui,
        InputContext::cutscene,
        InputContext::secondary,
    };
    for (const InputContext ctx : all_contexts) {
        if (action_active_in_context(a, ctx) &&
            action_active_in_context(b, ctx)) {
            out_shared = ctx;
            return true;
        }
    }
    return false;
}

} // namespace

// Native Windows XInput backend wrapped in move-only RAII so SDL3 stays
// strictly limited to window/events/Vulkan surface per AGENTS.md.
class HardwareGamepadBackend final {
public:
    HardwareGamepadBackend() noexcept {
#if defined(_WIN32)
        module_ = LoadLibraryExW(
            L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_ == nullptr) {
            module_ = LoadLibraryExW(
                L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (module_ != nullptr) {
            get_state_ = reinterpret_cast<GetStateFn>(
                GetProcAddress(module_, "XInputGetState"));
            set_state_ = reinterpret_cast<SetStateFn>(
                GetProcAddress(module_, "XInputSetState"));
        }
#endif
    }

    ~HardwareGamepadBackend() noexcept {
        stop_rumble();
#if defined(_WIN32)
        if (module_ != nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
#endif
    }

    HardwareGamepadBackend(const HardwareGamepadBackend&) = delete;
    HardwareGamepadBackend& operator=(const HardwareGamepadBackend&) = delete;

    [[nodiscard]] GamepadState poll(
        const float delta_seconds,
        const float trigger_threshold) noexcept {
        GamepadState state{};
#if defined(_WIN32)
        if (get_state_ == nullptr) {
            return state;
        }
        if (active_user_ == invalid_user) {
            reconnect_cooldown_ -= std::max(0.0F, delta_seconds);
            if (reconnect_cooldown_ > 0.0F) {
                return state;
            }
            for (DWORD user = 0U; user < XUSER_MAX_COUNT; ++user) {
                XINPUT_STATE xi{};
                if (get_state_(user, &xi) == ERROR_SUCCESS) {
                    active_user_ = user;
                    return decode_state(xi.Gamepad, trigger_threshold);
                }
            }
            // Avoid probing disconnected XInput slots every frame.
            reconnect_cooldown_ = 0.5F;
            return state;
        }

        XINPUT_STATE xi{};
        if (get_state_(active_user_, &xi) == ERROR_SUCCESS) {
            return decode_state(xi.Gamepad, trigger_threshold);
        }
        active_user_ = invalid_user;
        last_low_word_ = 0U;
        last_high_word_ = 0U;
        reconnect_cooldown_ = 0.5F;
#else
        (void)delta_seconds;
        (void)trigger_threshold;
#endif
        return state;
    }

    void set_rumble(const float low_freq, const float high_freq) noexcept {
#if defined(_WIN32)
        if (set_state_ == nullptr || active_user_ == invalid_user) {
            return;
        }
        const auto to_word = [](const float v) noexcept -> WORD {
            if (!std::isfinite(v)) {
                return 0U;
            }
            const float clamped = std::clamp(v, 0.0F, 1.0F);
            return static_cast<WORD>(clamped * 65535.0F + 0.5F);
        };
        const WORD low_word = to_word(low_freq);
        const WORD high_word = to_word(high_freq);
        if (low_word == last_low_word_ && high_word == last_high_word_) {
            return;
        }
        last_low_word_ = low_word;
        last_high_word_ = high_word;
        XINPUT_VIBRATION vib{};
        vib.wLeftMotorSpeed = low_word;
        vib.wRightMotorSpeed = high_word;
        static_cast<void>(set_state_(active_user_, &vib));
#else
        (void)low_freq;
        (void)high_freq;
#endif
    }

    void stop_rumble() noexcept {
#if defined(_WIN32)
        if (set_state_ != nullptr && active_user_ != invalid_user &&
            (last_low_word_ != 0U || last_high_word_ != 0U)) {
            XINPUT_VIBRATION vib{0U, 0U};
            static_cast<void>(set_state_(active_user_, &vib));
            last_low_word_ = 0U;
            last_high_word_ = 0U;
        }
#endif
    }

private:
#if defined(_WIN32)
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    static constexpr DWORD invalid_user = 0xFFFFFFFFU;

    [[nodiscard]] static float normalize_thumb(const SHORT v) noexcept {
        if (v >= 0) {
            return static_cast<float>(v) / 32767.0F;
        }
        return static_cast<float>(v) / 32768.0F;
    }

    [[nodiscard]] static GamepadState decode_state(
        const XINPUT_GAMEPAD& gp,
        const float trigger_threshold) noexcept {
        GamepadState out{};
        out.connected = true;
        out.left_stick = {
            normalize_thumb(gp.sThumbLX),
            normalize_thumb(gp.sThumbLY),
        };
        out.right_stick = {
            normalize_thumb(gp.sThumbRX),
            normalize_thumb(gp.sThumbRY),
        };
        out.left_trigger = static_cast<float>(gp.bLeftTrigger) / 255.0F;
        out.right_trigger = static_cast<float>(gp.bRightTrigger) / 255.0F;

        std::uint32_t mask = 0U;
        const auto map_btn = [&](const WORD xi_bit, const GamepadButton btn) {
            if ((gp.wButtons & xi_bit) != 0U) {
                mask |= gamepad_button_bit(btn);
            }
        };
        map_btn(XINPUT_GAMEPAD_A, GamepadButton::south);
        map_btn(XINPUT_GAMEPAD_B, GamepadButton::east);
        map_btn(XINPUT_GAMEPAD_X, GamepadButton::west);
        map_btn(XINPUT_GAMEPAD_Y, GamepadButton::north);
        map_btn(XINPUT_GAMEPAD_LEFT_SHOULDER, GamepadButton::left_shoulder);
        map_btn(XINPUT_GAMEPAD_RIGHT_SHOULDER, GamepadButton::right_shoulder);
        map_btn(XINPUT_GAMEPAD_BACK, GamepadButton::back);
        map_btn(XINPUT_GAMEPAD_START, GamepadButton::start);
        map_btn(XINPUT_GAMEPAD_LEFT_THUMB, GamepadButton::left_stick);
        map_btn(XINPUT_GAMEPAD_RIGHT_THUMB, GamepadButton::right_stick);
        map_btn(XINPUT_GAMEPAD_DPAD_UP, GamepadButton::dpad_up);
        map_btn(XINPUT_GAMEPAD_DPAD_DOWN, GamepadButton::dpad_down);
        map_btn(XINPUT_GAMEPAD_DPAD_LEFT, GamepadButton::dpad_left);
        map_btn(XINPUT_GAMEPAD_DPAD_RIGHT, GamepadButton::dpad_right);
        if (out.left_trigger >= trigger_threshold) {
            mask |= gamepad_button_bit(GamepadButton::left_trigger);
        }
        if (out.right_trigger >= trigger_threshold) {
            mask |= gamepad_button_bit(GamepadButton::right_trigger);
        }
        out.buttons_mask = mask;
        return out;
    }

    HMODULE module_ = nullptr;
    GetStateFn get_state_ = nullptr;
    SetStateFn set_state_ = nullptr;
    DWORD active_user_ = invalid_user;
    float reconnect_cooldown_ = 0.0F;
    WORD last_low_word_ = 0U;
    WORD last_high_word_ = 0U;
#endif
};

std::string_view to_token(const InputContext context) noexcept {
    switch (context) {
    case InputContext::gameplay: return "gameplay";
    case InputContext::ui: return "ui";
    case InputContext::cutscene: return "cutscene";
    case InputContext::secondary: return "secondary";
    }
    return "unknown";
}

std::optional<InputContext> parse_input_context(
    const std::string_view token) noexcept {
    if (token == "gameplay") return InputContext::gameplay;
    if (token == "ui") return InputContext::ui;
    if (token == "cutscene") return InputContext::cutscene;
    if (token == "secondary") return InputContext::secondary;
    return std::nullopt;
}

std::string_view to_token(const InputAction action) noexcept {
    const auto idx = static_cast<std::size_t>(action);
    if (idx < action_table.size()) {
        return action_table[idx].token;
    }
    return "unknown";
}

std::string_view input_action_display_name(const InputAction action) noexcept {
    const auto idx = static_cast<std::size_t>(action);
    if (idx < action_table.size()) {
        return action_table[idx].display_name;
    }
    return "Unknown";
}

std::optional<InputAction> parse_input_action(
    const std::string_view token) noexcept {
    for (const ActionMetadata& meta : action_table) {
        if (meta.token == token) {
            return meta.action;
        }
    }
    return std::nullopt;
}

bool action_active_in_context(
    const InputAction action,
    const InputContext context) noexcept {
    switch (action) {
    case InputAction::move_forward:
    case InputAction::move_backward:
    case InputAction::move_left:
    case InputAction::move_right:
    case InputAction::jump:
    case InputAction::run:
        return context == InputContext::gameplay;
    case InputAction::interact:
    case InputAction::pause:
    case InputAction::recover:
    case InputAction::quick_reset:
        return context == InputContext::gameplay ||
               context == InputContext::secondary;
    case InputAction::secondary_up:
    case InputAction::secondary_down:
    case InputAction::secondary_left:
    case InputAction::secondary_right:
    case InputAction::secondary_action:
        return context == InputContext::secondary;
    case InputAction::ui_up:
    case InputAction::ui_down:
    case InputAction::ui_left:
    case InputAction::ui_right:
    case InputAction::ui_confirm:
    case InputAction::ui_cancel:
        return context == InputContext::ui;
    case InputAction::cutscene_skip:
    case InputAction::cutscene_advance:
        return context == InputContext::cutscene;
    case InputAction::count:
        break;
    }
    return false;
}

std::string_view to_token(const InputKey key) noexcept {
    const auto idx = static_cast<std::size_t>(key);
    if (idx < key_tokens.size()) {
        return key_tokens[idx].token;
    }
    return "none";
}

std::optional<InputKey> parse_input_key(
    const std::string_view token) noexcept {
    for (const KeyTokenEntry& entry : key_tokens) {
        if (entry.token == token) {
            return entry.key;
        }
    }
    return std::nullopt;
}

std::string_view to_token(const GamepadButton button) noexcept {
    const auto idx = static_cast<std::size_t>(button);
    if (idx < gamepad_tokens.size()) {
        return gamepad_tokens[idx].token;
    }
    return "none";
}

std::optional<GamepadButton> parse_gamepad_button(
    const std::string_view token) noexcept {
    for (const GamepadTokenEntry& entry : gamepad_tokens) {
        if (entry.token == token) {
            return entry.button;
        }
    }
    return std::nullopt;
}

glm::vec2 apply_radial_deadzone(
    const glm::vec2 raw_stick,
    const float inner_deadzone,
    const float outer_deadzone) noexcept {
    if (!std::isfinite(raw_stick.x) || !std::isfinite(raw_stick.y)) {
        return {0.0F, 0.0F};
    }
    const float inner = std::clamp(inner_deadzone, 0.0F, 0.9F);
    const float outer = std::clamp(outer_deadzone, inner + 0.01F, 1.0F);
    const float length_sq =
        raw_stick.x * raw_stick.x + raw_stick.y * raw_stick.y;
    if (length_sq <= inner * inner) {
        return {0.0F, 0.0F};
    }
    const float length = std::sqrt(length_sq);
    const float remapped =
        std::clamp((length - inner) / (outer - inner), 0.0F, 1.0F);
    return raw_stick * (remapped / length);
}

InputActionSystem::InputActionSystem()
    : hardware_backend_(std::make_unique<HardwareGamepadBackend>()) {
    reset_bindings_to_default();
}

InputActionSystem::~InputActionSystem() noexcept = default;
InputActionSystem::InputActionSystem(InputActionSystem&&) noexcept = default;
InputActionSystem& InputActionSystem::operator=(InputActionSystem&&) noexcept =
    default;

void InputActionSystem::set_hardware_polling_enabled(
    const bool enabled) noexcept {
    if (hardware_polling_enabled_ && !enabled && hardware_backend_) {
        hardware_backend_->stop_rumble();
    }
    hardware_polling_enabled_ = enabled;
}

void InputActionSystem::set_context(const InputContext context) noexcept {
    if (context_ == context) {
        return;
    }
    context_ = context;
    clear_action_latches();
}

void InputActionSystem::set_gamepad_config(
    const GamepadConfig& config) noexcept {
    gamepad_config_ = sanitize_gamepad_config(config);
    if (!gamepad_config_.vibration_enabled) {
        stop_rumble();
    }
}

void InputActionSystem::reset_bindings_to_default() noexcept {
    for (std::size_t i = 0U; i < input_action_count; ++i) {
        bindings_[i] = action_table[i].default_binding;
    }
}

void InputActionSystem::set_binding(
    const InputAction action,
    ActionBinding binding) noexcept {
    const auto idx = static_cast<std::size_t>(action);
    if (idx >= input_action_count) {
        return;
    }
    if (static_cast<std::size_t>(binding.primary_key) >= input_key_count) {
        binding.primary_key = InputKey::none;
    }
    if (static_cast<std::size_t>(binding.secondary_key) >= input_key_count) {
        binding.secondary_key = InputKey::none;
    }
    if (static_cast<std::size_t>(binding.gamepad_button) >=
        gamepad_button_count) {
        binding.gamepad_button = GamepadButton::none;
    }
    if (binding.secondary_key == binding.primary_key) {
        binding.secondary_key = InputKey::none;
    }
    bindings_[idx] = binding;
}

ActionBinding InputActionSystem::binding(const InputAction action) const noexcept {
    const auto idx = static_cast<std::size_t>(action);
    if (idx >= input_action_count) {
        return {};
    }
    return bindings_[idx];
}

std::size_t InputActionSystem::detect_conflicts(
    const std::span<BindingConflict> out_conflicts) const noexcept {
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < input_action_count; ++i) {
        const auto a = static_cast<InputAction>(i);
        const ActionBinding& ba = bindings_[i];
        for (std::size_t j = i + 1U; j < input_action_count; ++j) {
            const auto b = static_cast<InputAction>(j);
            InputContext shared_ctx = InputContext::gameplay;
            if (!contexts_overlap(a, b, shared_ctx)) {
                continue;
            }
            const ActionBinding& bb = bindings_[j];
            InputKey shared_key = InputKey::none;
            if (ba.primary_key != InputKey::none &&
                (ba.primary_key == bb.primary_key ||
                 ba.primary_key == bb.secondary_key)) {
                shared_key = ba.primary_key;
            } else if (
                ba.secondary_key != InputKey::none &&
                (ba.secondary_key == bb.primary_key ||
                 ba.secondary_key == bb.secondary_key)) {
                shared_key = ba.secondary_key;
            }
            GamepadButton shared_gp = GamepadButton::none;
            if (ba.gamepad_button != GamepadButton::none &&
                ba.gamepad_button == bb.gamepad_button) {
                shared_gp = ba.gamepad_button;
            }
            if (shared_key != InputKey::none ||
                shared_gp != GamepadButton::none) {
                if (count < out_conflicts.size()) {
                    out_conflicts[count] = BindingConflict{
                        .first = a,
                        .second = b,
                        .shared_context = shared_ctx,
                        .shared_key = shared_key,
                        .shared_gamepad = shared_gp,
                    };
                }
                ++count;
            }
        }
    }
    return count;
}

void InputActionSystem::set_key_down(
    const InputKey key,
    const bool down) noexcept {
    const auto idx = static_cast<std::size_t>(key);
    if (key == InputKey::none || idx >= input_key_count) {
        return;
    }
    if (down && !keys_down_[idx]) {
        last_pressed_key_ = key;
    }
    keys_down_[idx] = down;
}

void InputActionSystem::pulse_key(const InputKey key) noexcept {
    const auto idx = static_cast<std::size_t>(key);
    if (key == InputKey::none || idx >= input_key_count) {
        return;
    }
    last_pressed_key_ = key;
    keys_pulsed_[idx] = true;
}

void InputActionSystem::add_mouse_delta(
    const float delta_x,
    const float delta_y) noexcept {
    if (std::isfinite(delta_x)) {
        pending_mouse_dx_ += delta_x;
    }
    if (std::isfinite(delta_y)) {
        pending_mouse_dy_ += delta_y;
    }
}

void InputActionSystem::set_synthetic_gamepad(
    std::optional<GamepadState> state) noexcept {
    synthetic_gamepad_ = state;
}

void InputActionSystem::release_all() noexcept {
    keys_down_.fill(false);
    keys_pulsed_.fill(false);
    pending_mouse_dx_ = 0.0F;
    pending_mouse_dy_ = 0.0F;
    last_pressed_key_ = InputKey::none;
    last_pressed_gamepad_button_ = GamepadButton::none;
    clear_action_latches();
    stop_rumble();
}

void InputActionSystem::clear_action_latches() noexcept {
    action_down_.fill(false);
    action_pressed_.fill(false);
    action_released_.fill(false);
    action_value_.fill(0.0F);
    move_axis_ = {0.0F, 0.0F};
    secondary_axis_ = {0.0F, 0.0F};
    look_delta_ = {0.0F, 0.0F};
    ui_nav_step_ = {0.0F, 0.0F};
    ui_repeat_timer_ = 0.0F;
    ui_stick_latched_x_ = false;
    ui_stick_latched_y_ = false;
}

void InputActionSystem::begin_frame(const float delta_seconds) noexcept {
    const float dt =
        std::isfinite(delta_seconds) && delta_seconds > 0.0F
            ? std::min(delta_seconds, 0.1F)
            : 0.0F;

    if (synthetic_gamepad_) {
        raw_gamepad_ = *synthetic_gamepad_;
        if (raw_gamepad_.connected) {
            if (raw_gamepad_.left_trigger >= gamepad_config_.trigger_threshold) {
                raw_gamepad_.buttons_mask |=
                    gamepad_button_bit(GamepadButton::left_trigger);
            }
            if (raw_gamepad_.right_trigger >= gamepad_config_.trigger_threshold) {
                raw_gamepad_.buttons_mask |=
                    gamepad_button_bit(GamepadButton::right_trigger);
            }
        }
    } else if (hardware_polling_enabled_ && hardware_backend_) {
        raw_gamepad_ =
            hardware_backend_->poll(dt, gamepad_config_.trigger_threshold);
    } else {
        raw_gamepad_ = {};
    }

    filtered_gamepad_ = raw_gamepad_;
    if (filtered_gamepad_.connected) {
        filtered_gamepad_.left_stick = apply_radial_deadzone(
            raw_gamepad_.left_stick,
            gamepad_config_.stick_inner_deadzone,
            gamepad_config_.stick_outer_deadzone);
        filtered_gamepad_.right_stick = apply_radial_deadzone(
            raw_gamepad_.right_stick,
            gamepad_config_.stick_inner_deadzone,
            gamepad_config_.stick_outer_deadzone);
        const std::uint32_t newly_pressed =
            filtered_gamepad_.buttons_mask & ~previous_gamepad_buttons_;
        if (newly_pressed != 0U) {
            for (std::size_t b = 1U; b < gamepad_button_count; ++b) {
                const auto btn = static_cast<GamepadButton>(b);
                if ((newly_pressed & gamepad_button_bit(btn)) != 0U) {
                    last_pressed_gamepad_button_ = btn;
                    break;
                }
            }
        }
        previous_gamepad_buttons_ = filtered_gamepad_.buttons_mask;
    } else {
        filtered_gamepad_.left_stick = {0.0F, 0.0F};
        filtered_gamepad_.right_stick = {0.0F, 0.0F};
        filtered_gamepad_.left_trigger = 0.0F;
        filtered_gamepad_.right_trigger = 0.0F;
        filtered_gamepad_.buttons_mask = 0U;
        previous_gamepad_buttons_ = 0U;
    }

    const auto key_active = [this](const InputKey key) noexcept {
        const auto idx = static_cast<std::size_t>(key);
        return key != InputKey::none && idx < input_key_count &&
               (keys_down_[idx] || keys_pulsed_[idx]);
    };
    const auto is_analog_trigger = [](const GamepadButton btn) noexcept -> bool {
        return btn == GamepadButton::left_trigger || btn == GamepadButton::right_trigger;
    };
    const auto analog_trigger_value = [&](const GamepadButton btn) noexcept -> float {
        if (!filtered_gamepad_.connected) {
            return 0.0F;
        }
        if (btn == GamepadButton::left_trigger) {
            return filtered_gamepad_.left_trigger;
        }
        if (btn == GamepadButton::right_trigger) {
            return filtered_gamepad_.right_trigger;
        }
        return 0.0F;
    };

    for (std::size_t i = 0U; i < input_action_count; ++i) {
        const auto act = static_cast<InputAction>(i);
        const bool active = action_active_in_context(act, context_);
        const bool was_down = action_down_[i];
        bool now_down = false;
        float analog_val = 0.0F;
        if (active) {
            const ActionBinding& bind = bindings_[i];
            const bool key_or_pad =
                key_active(bind.primary_key) ||
                key_active(bind.secondary_key) ||
                (filtered_gamepad_.connected &&
                 !is_analog_trigger(bind.gamepad_button) &&
                 filtered_gamepad_.button_down(bind.gamepad_button));
            const float trig_val = analog_trigger_value(bind.gamepad_button);
            now_down = key_or_pad ||
                       (filtered_gamepad_.connected &&
                        filtered_gamepad_.button_down(bind.gamepad_button));
            analog_val = std::clamp(
                std::max(key_or_pad ? 1.0F : 0.0F, trig_val), 0.0F, 1.0F);
        }
        action_down_[i] = now_down;
        action_pressed_[i] = now_down && !was_down;
        action_released_[i] = !now_down && was_down;
        action_value_[i] = analog_val;
    }

    keys_pulsed_.fill(false);

    if (context_ == InputContext::gameplay) {
        const float digital_x =
            static_cast<float>(action_down(InputAction::move_right)) -
            static_cast<float>(action_down(InputAction::move_left));
        const float digital_y =
            static_cast<float>(action_down(InputAction::move_forward)) -
            static_cast<float>(action_down(InputAction::move_backward));
        glm::vec2 combined = filtered_gamepad_.left_stick +
                             glm::vec2{digital_x, digital_y};
        const float len_sq =
            combined.x * combined.x + combined.y * combined.y;
        if (len_sq > 1.0F) {
            combined *= 1.0F / std::sqrt(len_sq);
        }
        move_axis_ = combined;
        secondary_axis_ = {0.0F, 0.0F};
    } else if (context_ == InputContext::secondary) {
        move_axis_ = {0.0F, 0.0F};
        const float digital_x =
            static_cast<float>(action_down(InputAction::secondary_right)) -
            static_cast<float>(action_down(InputAction::secondary_left));
        const float digital_y =
            static_cast<float>(action_down(InputAction::secondary_up)) -
            static_cast<float>(action_down(InputAction::secondary_down));
        secondary_axis_ = {
            std::clamp(digital_x + filtered_gamepad_.left_stick.x, -1.0F, 1.0F),
            std::clamp(digital_y + filtered_gamepad_.left_stick.y, -1.0F, 1.0F),
        };
    } else {
        move_axis_ = {0.0F, 0.0F};
        secondary_axis_ = {0.0F, 0.0F};
    }

    if (context_ == InputContext::gameplay ||
        context_ == InputContext::secondary) {
        const float stick_look_x =
            filtered_gamepad_.right_stick.x *
            gamepad_config_.look_speed_pixels_per_sec * dt;
        const float stick_look_y_sign =
            gamepad_config_.invert_look_y ? 1.0F : -1.0F;
        const float stick_look_y =
            filtered_gamepad_.right_stick.y * stick_look_y_sign *
            gamepad_config_.look_speed_pixels_per_sec * dt;
        look_delta_ = {
            pending_mouse_dx_ + stick_look_x,
            pending_mouse_dy_ + stick_look_y,
        };
    } else {
        look_delta_ = {0.0F, 0.0F};
    }
    pending_mouse_dx_ = 0.0F;
    pending_mouse_dy_ = 0.0F;

    ui_nav_step_ = {0.0F, 0.0F};
    if (context_ == InputContext::ui) {
        float step_x =
            static_cast<float>(action_pressed(InputAction::ui_right)) -
            static_cast<float>(action_pressed(InputAction::ui_left));
        float step_y =
            static_cast<float>(action_pressed(InputAction::ui_up)) -
            static_cast<float>(action_pressed(InputAction::ui_down));
        const glm::vec2 stick = filtered_gamepad_.left_stick;
        constexpr float nav_threshold = 0.55F;
        if (std::abs(stick.x) < 0.3F) {
            ui_stick_latched_x_ = false;
        }
        if (std::abs(stick.y) < 0.3F) {
            ui_stick_latched_y_ = false;
        }
        if (ui_repeat_timer_ > 0.0F) {
            ui_repeat_timer_ = std::max(0.0F, ui_repeat_timer_ - dt);
        }
        if (step_x == 0.0F && std::abs(stick.x) >= nav_threshold &&
            (!ui_stick_latched_x_ || ui_repeat_timer_ <= 0.0F)) {
            step_x = stick.x > 0.0F ? 1.0F : -1.0F;
            ui_stick_latched_x_ = true;
            ui_repeat_timer_ = 0.22F;
        }
        if (step_y == 0.0F && std::abs(stick.y) >= nav_threshold &&
            (!ui_stick_latched_y_ || ui_repeat_timer_ <= 0.0F)) {
            step_y = stick.y > 0.0F ? 1.0F : -1.0F;
            ui_stick_latched_y_ = true;
            ui_repeat_timer_ = 0.22F;
        }
        ui_nav_step_ = {step_x, step_y};
    }

    if (rumble_.remaining_seconds > 0.0F) {
        rumble_.remaining_seconds =
            std::max(0.0F, rumble_.remaining_seconds - dt);
        if (rumble_.remaining_seconds <= 0.0F ||
            !gamepad_config_.vibration_enabled) {
            rumble_ = {};
            push_hardware_rumble(0.0F, 0.0F);
        } else {
            push_hardware_rumble(
                rumble_.low_frequency, rumble_.high_frequency);
        }
    } else {
        push_hardware_rumble(0.0F, 0.0F);
    }
}

bool InputActionSystem::action_down(const InputAction action) const noexcept {
    const auto idx = static_cast<std::size_t>(action);
    return idx < input_action_count && action_down_[idx];
}

bool InputActionSystem::action_pressed(const InputAction action) const noexcept {
    const auto idx = static_cast<std::size_t>(action);
    return idx < input_action_count && action_pressed_[idx];
}

bool InputActionSystem::action_released(
    const InputAction action) const noexcept {
    const auto idx = static_cast<std::size_t>(action);
    return idx < input_action_count && action_released_[idx];
}

float InputActionSystem::action_value(const InputAction action) const noexcept {
    const auto idx = static_cast<std::size_t>(action);
    return idx < input_action_count ? action_value_[idx] : 0.0F;
}

InputKey InputActionSystem::consume_last_pressed_key() noexcept {
    return std::exchange(last_pressed_key_, InputKey::none);
}

GamepadButton InputActionSystem::consume_last_pressed_gamepad_button() noexcept {
    return std::exchange(last_pressed_gamepad_button_, GamepadButton::none);
}

void InputActionSystem::trigger_rumble(
    const float low_frequency,
    const float high_frequency,
    const float duration_seconds) noexcept {
    if (!gamepad_config_.vibration_enabled ||
        !std::isfinite(duration_seconds) ||
        duration_seconds <= 0.0F) {
        return;
    }
    const float low = std::isfinite(low_frequency)
        ? std::clamp(low_frequency, 0.0F, 1.0F)
        : 0.0F;
    const float high = std::isfinite(high_frequency)
        ? std::clamp(high_frequency, 0.0F, 1.0F)
        : 0.0F;
    if (low <= 0.0F && high <= 0.0F) {
        return;
    }
    rumble_.low_frequency = std::max(rumble_.low_frequency, low);
    rumble_.high_frequency = std::max(rumble_.high_frequency, high);
    rumble_.remaining_seconds = std::max(
        rumble_.remaining_seconds, std::min(duration_seconds, 5.0F));
    push_hardware_rumble(rumble_.low_frequency, rumble_.high_frequency);
}

void InputActionSystem::stop_rumble() noexcept {
    rumble_ = {};
    push_hardware_rumble(0.0F, 0.0F);
}

void InputActionSystem::push_hardware_rumble(
    const float low_freq,
    const float high_freq) noexcept {
    if (!hardware_polling_enabled_ || !hardware_backend_) {
        return;
    }
    hardware_backend_->set_rumble(low_freq, high_freq);
}

bool save_input_bindings_file(
    const std::filesystem::path& file_path,
    const InputActionSystem& system,
    std::string& error) {
    error.clear();
    if (file_path.empty()) {
        error = "Input bindings path is empty.";
        return false;
    }
    std::error_code fs_err;
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path(), fs_err);
        if (fs_err) {
            error = "Unable to create directory for input bindings.";
            return false;
        }
    }

    Json actions_json = Json::object();
    for (std::size_t i = 0U; i < input_action_count; ++i) {
        const auto act = static_cast<InputAction>(i);
        const ActionBinding bind = system.binding(act);
        actions_json[std::string(to_token(act))] = Json{
            {"primary_key", to_token(bind.primary_key)},
            {"secondary_key", to_token(bind.secondary_key)},
            {"gamepad_button", to_token(bind.gamepad_button)},
        };
    }
    const GamepadConfig& gp = system.gamepad_config();
    const Json doc{
        {"schema", bindings_schema},
        {"version", bindings_schema_version},
        {"gamepad",
         {
             {"stick_inner_deadzone", gp.stick_inner_deadzone},
             {"stick_outer_deadzone", gp.stick_outer_deadzone},
             {"trigger_threshold", gp.trigger_threshold},
             {"look_speed_pixels_per_sec", gp.look_speed_pixels_per_sec},
             {"invert_look_y", gp.invert_look_y},
             {"vibration_enabled", gp.vibration_enabled},
         }},
        {"actions", std::move(actions_json)},
    };

    const std::filesystem::path temp_path =
        file_path.string() + ".tmp";
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Unable to open temporary input bindings file for writing.";
            return false;
        }
        const std::string serialized = doc.dump(2) + "\n";
        out.write(
            serialized.data(),
            static_cast<std::streamsize>(serialized.size()));
        out.flush();
        if (!out) {
            error = "Failed to flush temporary input bindings file.";
            std::filesystem::remove(temp_path, fs_err);
            return false;
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(
            temp_path.c_str(),
            file_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temp_path, fs_err);
        error = "Failed to atomically replace input bindings file.";
        return false;
    }
#else
    std::filesystem::rename(temp_path, file_path, fs_err);
    if (fs_err) {
        std::filesystem::remove(temp_path, fs_err);
        error = "Failed to rename temporary input bindings file.";
        return false;
    }
#endif
    return true;
}

bool load_input_bindings_file(
    const std::filesystem::path& file_path,
    InputActionSystem& system,
    std::string& error) {
    error.clear();
    std::error_code fs_err;
    if (!std::filesystem::is_regular_file(file_path, fs_err) || fs_err) {
        error = "Input bindings file does not exist.";
        return false;
    }
    const std::uintmax_t bytes = std::filesystem::file_size(file_path, fs_err);
    if (fs_err || bytes == 0U || bytes > 1024ULL * 1024ULL) {
        error = "Input bindings file is empty or exceeds 1 MiB.";
        return false;
    }
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        error = "Unable to open input bindings file.";
        return false;
    }
    const Json doc = Json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() ||
        doc.value("schema", std::string{}) != bindings_schema ||
        doc.value("version", 0U) != bindings_schema_version ||
        !doc.contains("actions") || !doc["actions"].is_object()) {
        error = "Invalid input bindings JSON schema or version.";
        return false;
    }

    std::array<ActionBinding, input_action_count> staged{};
    for (std::size_t i = 0U; i < input_action_count; ++i) {
        staged[i] = action_table[i].default_binding;
    }

    for (const auto& [action_key, entry] : doc["actions"].items()) {
        const auto parsed_action = parse_input_action(action_key);
        if (!parsed_action || !entry.is_object()) {
            error = "Unknown action entry in input bindings file: " + action_key;
            return false;
        }
        ActionBinding bind = staged[static_cast<std::size_t>(*parsed_action)];
        if (entry.contains("primary_key")) {
            if (!entry["primary_key"].is_string()) {
                error = "primary_key must be a string.";
                return false;
            }
            const auto k = parse_input_key(
                entry["primary_key"].get<std::string_view>());
            if (!k) {
                error = "Unknown primary_key token.";
                return false;
            }
            bind.primary_key = *k;
        }
        if (entry.contains("secondary_key")) {
            if (!entry["secondary_key"].is_string()) {
                error = "secondary_key must be a string.";
                return false;
            }
            const auto k = parse_input_key(
                entry["secondary_key"].get<std::string_view>());
            if (!k) {
                error = "Unknown secondary_key token.";
                return false;
            }
            bind.secondary_key = *k;
        }
        if (entry.contains("gamepad_button")) {
            if (!entry["gamepad_button"].is_string()) {
                error = "gamepad_button must be a string.";
                return false;
            }
            const auto b = parse_gamepad_button(
                entry["gamepad_button"].get<std::string_view>());
            if (!b) {
                error = "Unknown gamepad_button token.";
                return false;
            }
            bind.gamepad_button = *b;
        }
        staged[static_cast<std::size_t>(*parsed_action)] = bind;
    }

    GamepadConfig gp = system.gamepad_config();
    if (doc.contains("gamepad")) {
        if (!doc["gamepad"].is_object()) {
            error = "gamepad section must be an object.";
            return false;
        }
        const Json& g = doc["gamepad"];
        if (g.contains("stick_inner_deadzone") &&
            g["stick_inner_deadzone"].is_number()) {
            gp.stick_inner_deadzone = g["stick_inner_deadzone"].get<float>();
        }
        if (g.contains("stick_outer_deadzone") &&
            g["stick_outer_deadzone"].is_number()) {
            gp.stick_outer_deadzone = g["stick_outer_deadzone"].get<float>();
        }
        if (g.contains("trigger_threshold") &&
            g["trigger_threshold"].is_number()) {
            gp.trigger_threshold = g["trigger_threshold"].get<float>();
        }
        if (g.contains("look_speed_pixels_per_sec") &&
            g["look_speed_pixels_per_sec"].is_number()) {
            gp.look_speed_pixels_per_sec =
                g["look_speed_pixels_per_sec"].get<float>();
        }
        if (g.contains("invert_look_y") && g["invert_look_y"].is_boolean()) {
            gp.invert_look_y = g["invert_look_y"].get<bool>();
        }
        if (g.contains("vibration_enabled") &&
            g["vibration_enabled"].is_boolean()) {
            gp.vibration_enabled = g["vibration_enabled"].get<bool>();
        }
    }

    for (std::size_t i = 0U; i < input_action_count; ++i) {
        system.set_binding(static_cast<InputAction>(i), staged[i]);
    }
    system.set_gamepad_config(gp);
    return true;
}

} // namespace vcr
