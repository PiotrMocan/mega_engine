#pragma once

#include "audio_engine.hpp"
#include "thunderstorm.hpp"

namespace vcr {
inline void update_storm_audio(Thunderstorm& storm, AudioEngine& audio, bool preview) noexcept {
    if (storm.take_cancel()) audio.stop_thunder();
    while (const auto event = storm.take_sound())
        audio.play_thunder(event->position, event->volume, event->duration, event->seed, preview);
}
} // namespace vcr
