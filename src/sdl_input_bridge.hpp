#pragma once

#include "input_actions.hpp"

#include <SDL3/SDL.h>

namespace vcr {

[[nodiscard]] inline InputKey input_key_from_sdl_keycode(
    const SDL_Keycode key) noexcept {
    switch (key) {
    case SDLK_A: return InputKey::a;
    case SDLK_B: return InputKey::b;
    case SDLK_C: return InputKey::c;
    case SDLK_D: return InputKey::d;
    case SDLK_E: return InputKey::e;
    case SDLK_F: return InputKey::f;
    case SDLK_G: return InputKey::g;
    case SDLK_H: return InputKey::h;
    case SDLK_I: return InputKey::i;
    case SDLK_J: return InputKey::j;
    case SDLK_K: return InputKey::k;
    case SDLK_L: return InputKey::l;
    case SDLK_M: return InputKey::m;
    case SDLK_N: return InputKey::n;
    case SDLK_O: return InputKey::o;
    case SDLK_P: return InputKey::p;
    case SDLK_Q: return InputKey::q;
    case SDLK_R: return InputKey::r;
    case SDLK_S: return InputKey::s;
    case SDLK_T: return InputKey::t;
    case SDLK_U: return InputKey::u;
    case SDLK_V: return InputKey::v;
    case SDLK_W: return InputKey::w;
    case SDLK_X: return InputKey::x;
    case SDLK_Y: return InputKey::y;
    case SDLK_Z: return InputKey::z;
    case SDLK_0: return InputKey::digit_0;
    case SDLK_1: return InputKey::digit_1;
    case SDLK_2: return InputKey::digit_2;
    case SDLK_3: return InputKey::digit_3;
    case SDLK_4: return InputKey::digit_4;
    case SDLK_5: return InputKey::digit_5;
    case SDLK_6: return InputKey::digit_6;
    case SDLK_7: return InputKey::digit_7;
    case SDLK_8: return InputKey::digit_8;
    case SDLK_9: return InputKey::digit_9;
    case SDLK_SPACE: return InputKey::space;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return InputKey::enter;
    case SDLK_ESCAPE: return InputKey::escape;
    case SDLK_TAB: return InputKey::tab;
    case SDLK_BACKSPACE: return InputKey::backspace;
    case SDLK_LSHIFT: return InputKey::left_shift;
    case SDLK_RSHIFT: return InputKey::right_shift;
    case SDLK_LCTRL: return InputKey::left_ctrl;
    case SDLK_RCTRL: return InputKey::right_ctrl;
    case SDLK_LALT: return InputKey::left_alt;
    case SDLK_RALT: return InputKey::right_alt;
    case SDLK_UP: return InputKey::up;
    case SDLK_DOWN: return InputKey::down;
    case SDLK_LEFT: return InputKey::left;
    case SDLK_RIGHT: return InputKey::right;
    case SDLK_F1: return InputKey::f1;
    case SDLK_F2: return InputKey::f2;
    case SDLK_F3: return InputKey::f3;
    case SDLK_F4: return InputKey::f4;
    case SDLK_F5: return InputKey::f5;
    case SDLK_F6: return InputKey::f6;
    case SDLK_F7: return InputKey::f7;
    case SDLK_F8: return InputKey::f8;
    case SDLK_F9: return InputKey::f9;
    case SDLK_F10: return InputKey::f10;
    case SDLK_F11: return InputKey::f11;
    case SDLK_F12: return InputKey::f12;
    default: return InputKey::none;
    }
}

[[nodiscard]] inline InputKey input_key_from_sdl_mouse_button(
    const Uint8 button) noexcept {
    switch (button) {
    case SDL_BUTTON_LEFT: return InputKey::mouse_left;
    case SDL_BUTTON_RIGHT: return InputKey::mouse_right;
    case SDL_BUTTON_MIDDLE: return InputKey::mouse_middle;
    case SDL_BUTTON_X1: return InputKey::mouse_x1;
    case SDL_BUTTON_X2: return InputKey::mouse_x2;
    default: return InputKey::none;
    }
}

} // namespace vcr
