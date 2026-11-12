#pragma once

#include <glm/vec2.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vcr {

enum class InputContext : std::uint8_t {
    gameplay = 0U,
    ui = 1U,
    cutscene = 2U,
};

enum class InputAction : std::uint8_t {
    move_forward = 0U,
    move_backward,
    move_left,
    move_right,
    jump,
    run,
    interact,
    pause,
    ui_up,
    ui_down,
    ui_left,
    ui_right,
    ui_confirm,
    ui_cancel,
    cutscene_skip,
    cutscene_advance,
    count,
};

inline constexpr std::size_t input_action_count =
    static_cast<std::size_t>(InputAction::count);

enum class InputKey : std::uint8_t {
    none = 0U,
    a,
    b,
    c,
    d,
    e,
    f,
    g,
    h,
    i,
    j,
    k,
    l,
    m,
    n,
    o,
    p,
    q,
    r,
    s,
    t,
    u,
    v,
    w,
    x,
    y,
    z,
    digit_0,
    digit_1,
    digit_2,
    digit_3,
    digit_4,
    digit_5,
    digit_6,
    digit_7,
    digit_8,
    digit_9,
    space,
    enter,
    escape,
    tab,
    backspace,
    left_shift,
    right_shift,
    left_ctrl,
    right_ctrl,
    left_alt,
    right_alt,
    up,
    down,
    left,
    right,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    mouse_left,
    mouse_right,
    mouse_middle,
    mouse_x1,
    mouse_x2,
    count,
};

inline constexpr std::size_t input_key_count =
    static_cast<std::size_t>(InputKey::count);

enum class GamepadButton : std::uint8_t {
    none = 0U,
    south,
    east,
    west,
    north,
    left_shoulder,
    right_shoulder,
    back,
    start,
    left_stick,
    right_stick,
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    left_trigger,
    right_trigger,
    count,
};

inline constexpr std::size_t gamepad_button_count =
    static_cast<std::size_t>(GamepadButton::count);

[[nodiscard]] constexpr std::uint32_t gamepad_button_bit(
    const GamepadButton button) noexcept {
    if (button == GamepadButton::none ||
        button == GamepadButton::count) {
        return 0U;
    }
    return 1U << static_cast<std::uint32_t>(button);
}

struct ActionBinding {
    InputKey primary_key = InputKey::none;
    InputKey secondary_key = InputKey::none;
    GamepadButton gamepad_button = GamepadButton::none;

    friend bool operator==(
        const ActionBinding&,
        const ActionBinding&) = default;
};

struct GamepadState {
    glm::vec2 left_stick{0.0F, 0.0F};
    glm::vec2 right_stick{0.0F, 0.0F};
    float left_trigger = 0.0F;
    float right_trigger = 0.0F;
    std::uint32_t buttons_mask = 0U;
    bool connected = false;

    [[nodiscard]] bool button_down(const GamepadButton button) const noexcept {
        const std::uint32_t bit = gamepad_button_bit(button);
        return bit != 0U && (buttons_mask & bit) != 0U;
    }
};

struct GamepadConfig {
    float stick_inner_deadzone = 0.18F;
    float stick_outer_deadzone = 0.95F;
    float trigger_threshold = 0.35F;
    float look_speed_pixels_per_sec = 720.0F;
    bool invert_look_y = false;
    bool vibration_enabled = true;
};

struct RumbleMotorState {
    float low_frequency = 0.0F;
    float high_frequency = 0.0F;
    float remaining_seconds = 0.0F;
};

struct BindingConflict {
    InputAction first = InputAction::move_forward;
    InputAction second = InputAction::move_forward;
    InputContext shared_context = InputContext::gameplay;
    InputKey shared_key = InputKey::none;
    GamepadButton shared_gamepad = GamepadButton::none;
};

[[nodiscard]] std::string_view to_token(InputContext context) noexcept;
[[nodiscard]] std::optional<InputContext> parse_input_context(
    std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(InputAction action) noexcept;
[[nodiscard]] std::string_view input_action_display_name(
    InputAction action) noexcept;
[[nodiscard]] std::optional<InputAction> parse_input_action(
    std::string_view token) noexcept;
[[nodiscard]] bool action_active_in_context(
    InputAction action,
    InputContext context) noexcept;

[[nodiscard]] std::string_view to_token(InputKey key) noexcept;
[[nodiscard]] std::optional<InputKey> parse_input_key(
    std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(GamepadButton button) noexcept;
[[nodiscard]] std::optional<GamepadButton> parse_gamepad_button(
    std::string_view token) noexcept;

[[nodiscard]] glm::vec2 apply_radial_deadzone(
    glm::vec2 raw_stick,
    float inner_deadzone,
    float outer_deadzone) noexcept;

class HardwareGamepadBackend;

class InputActionSystem final {
public:
    InputActionSystem();
    ~InputActionSystem() noexcept;

    InputActionSystem(const InputActionSystem&) = delete;
    InputActionSystem& operator=(const InputActionSystem&) = delete;
    InputActionSystem(InputActionSystem&&) noexcept;
    InputActionSystem& operator=(InputActionSystem&&) noexcept;

    void set_hardware_polling_enabled(bool enabled) noexcept;
    [[nodiscard]] bool hardware_polling_enabled() const noexcept {
        return hardware_polling_enabled_;
    }

    void set_context(InputContext context) noexcept;
    [[nodiscard]] InputContext context() const noexcept { return context_; }

    void set_gamepad_config(const GamepadConfig& config) noexcept;
    [[nodiscard]] const GamepadConfig& gamepad_config() const noexcept {
        return gamepad_config_;
    }

    void reset_bindings_to_default() noexcept;
    void set_binding(InputAction action, ActionBinding binding) noexcept;
    [[nodiscard]] ActionBinding binding(InputAction action) const noexcept;
    [[nodiscard]] const std::array<ActionBinding, input_action_count>&
    bindings() const noexcept {
        return bindings_;
    }
    [[nodiscard]] std::size_t detect_conflicts(
        std::span<BindingConflict> out_conflicts) const noexcept;

    void set_key_down(InputKey key, bool down) noexcept;
    void pulse_key(InputKey key) noexcept;
    void add_mouse_delta(float delta_x, float delta_y) noexcept;
    void set_synthetic_gamepad(std::optional<GamepadState> state) noexcept;
    void release_all() noexcept;

    // Evaluates all raw inputs against the active InputContext, updates edge
    // transitions, composite analog axes, and gamepad rumble motor timers.
    void begin_frame(float delta_seconds) noexcept;

    [[nodiscard]] bool action_down(InputAction action) const noexcept;
    [[nodiscard]] bool action_pressed(InputAction action) const noexcept;
    [[nodiscard]] bool action_released(InputAction action) const noexcept;

    // In Gameplay context: X in [-1, 1] (right), Y in [-1, 1] (forward).
    // Preserves partial analog tilt (< 1) and clamps length <= 1.
    [[nodiscard]] glm::vec2 move_axis() const noexcept { return move_axis_; }

    // Effective look delta in mouse-equivalent units (combines accumulated
    // mouse motion + right gamepad stick * look_speed_pixels_per_sec * dt).
    [[nodiscard]] glm::vec2 look_delta() const noexcept { return look_delta_; }

    // In UI context: discrete navigation steps (-1, 0, +1) with stick/key edge
    // repeat cooldown so menus navigate cleanly on both keyboard and gamepad.
    [[nodiscard]] glm::vec2 ui_navigation_step() const noexcept {
        return ui_nav_step_;
    }

    [[nodiscard]] const GamepadState& gamepad_state() const noexcept {
        return filtered_gamepad_;
    }
    [[nodiscard]] InputKey consume_last_pressed_key() noexcept;
    [[nodiscard]] GamepadButton consume_last_pressed_gamepad_button() noexcept;

    void trigger_rumble(
        float low_frequency,
        float high_frequency,
        float duration_seconds) noexcept;
    void stop_rumble() noexcept;
    [[nodiscard]] RumbleMotorState rumble_state() const noexcept {
        return rumble_;
    }

private:
    void clear_action_latches() noexcept;
    void push_hardware_rumble(float low_freq, float high_freq) noexcept;

    std::array<ActionBinding, input_action_count> bindings_{};
    std::array<bool, input_key_count> keys_down_{};
    std::array<bool, input_key_count> keys_pulsed_{};
    std::array<bool, input_action_count> action_down_{};
    std::array<bool, input_action_count> action_pressed_{};
    std::array<bool, input_action_count> action_released_{};
    GamepadConfig gamepad_config_{};
    std::optional<GamepadState> synthetic_gamepad_{};
    GamepadState raw_gamepad_{};
    GamepadState filtered_gamepad_{};
    std::uint32_t previous_gamepad_buttons_ = 0U;
    InputKey last_pressed_key_ = InputKey::none;
    GamepadButton last_pressed_gamepad_button_ = GamepadButton::none;
    float pending_mouse_dx_ = 0.0F;
    float pending_mouse_dy_ = 0.0F;
    glm::vec2 move_axis_{0.0F, 0.0F};
    glm::vec2 look_delta_{0.0F, 0.0F};
    glm::vec2 ui_nav_step_{0.0F, 0.0F};
    float ui_repeat_timer_ = 0.0F;
    bool ui_stick_latched_x_ = false;
    bool ui_stick_latched_y_ = false;
    RumbleMotorState rumble_{};
    InputContext context_ = InputContext::gameplay;
    bool hardware_polling_enabled_ = false;
    std::unique_ptr<HardwareGamepadBackend> hardware_backend_;
};

[[nodiscard]] bool save_input_bindings_file(
    const std::filesystem::path& file_path,
    const InputActionSystem& system,
    std::string& error);

[[nodiscard]] bool load_input_bindings_file(
    const std::filesystem::path& file_path,
    InputActionSystem& system,
    std::string& error);

} // namespace vcr
