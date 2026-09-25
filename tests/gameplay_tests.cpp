#include "gameplay.hpp"

#include <cmath>
#include <iostream>
#include <limits>

#include <glm/geometric.hpp>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] bool close_to(
    const float value,
    const float expected,
    const float epsilon = 0.0001F) {
    return std::abs(value - expected) <= epsilon;
}

[[nodiscard]] float planar_distance(const glm::vec3 value) {
    return std::sqrt(value.x * value.x + value.z * value.z);
}

} // namespace

int main() {
    const glm::vec3 camera_forward{0.0F, 0.0F, -1.0F};

    {
        vcr::CharacterController walking;
        walking.reset({}, 0.0F);
        vcr::CharacterInput input{.forward = true};
        for (int step = 0; step < 20; ++step) {
            walking.update(input, camera_forward, 0.05F);
        }
        expect(
            close_to(walking.position().z, -3.5F),
            "W moves the character at walk speed relative to the camera");
        expect(
            std::abs(walking.facing_yaw()) > 0.1F,
            "movement turns the character toward its travel direction");

        vcr::CharacterController running;
        running.reset({}, 3.14159265359F);
        input.run = true;
        for (int step = 0; step < 20; ++step) {
            running.update(input, camera_forward, 0.05F);
        }
        expect(
            close_to(running.position().z, -7.0F),
            "Shift run uses the configured run speed");
    }

    {
        vcr::CharacterController straight;
        vcr::CharacterController diagonal;
        straight.reset({}, 3.14159265359F);
        diagonal.reset({}, 3.14159265359F);
        vcr::CharacterInput input{.forward = true};
        straight.update(input, camera_forward, 0.05F);
        input.right = true;
        diagonal.update(input, camera_forward, 0.05F);
        expect(
            close_to(
                planar_distance(straight.position()),
                planar_distance(diagonal.position())),
            "diagonal character movement is normalized");

        input = {
            .forward = true,
            .backward = true,
            .left = true,
            .right = true,
        };
        const glm::vec3 before = diagonal.position();
        diagonal.update(input, camera_forward, 0.05F);
        expect(
            glm::length(diagonal.position() - before) < 0.0001F,
            "opposing character inputs cancel");
    }

    {
        vcr::CharacterController character(vcr::CharacterConfig{
            .ground_height = -2.0F,
        });
        character.reset({0.0F, -2.0F, 0.0F}, 3.14159265359F);
        vcr::CharacterInput input{.jump = true};
        character.update(input, camera_forward, 0.05F);
        expect(
            !character.grounded() && character.position().y > -2.0F &&
                character.vertical_velocity() > 0.0F,
            "Space starts an upward jump from the authored ground height");
        for (int step = 0; step < 100; ++step) {
            character.update(input, camera_forward, 0.05F);
        }
        expect(
            character.grounded() && close_to(character.position().y, -2.0F),
            "holding Space lands without automatic repeated jumps");
        input.jump = false;
        character.update(input, camera_forward, 0.05F);
        input.jump = true;
        character.update(input, camera_forward, 0.05F);
        expect(
            !character.grounded() && character.position().y > -2.0F,
            "releasing Space arms the next jump");
    }

    {
        vcr::CharacterController character;
        character.reset({}, 3.14159265359F);
        const vcr::CharacterInput input{.forward = true};
        character.update(input, camera_forward, 1.0F);
        expect(
            close_to(character.position().z, -0.175F),
            "character simulation clamps long frame deltas");
        const glm::vec3 before = character.position();
        character.update(input, camera_forward, -1.0F);
        character.update(
            input,
            camera_forward,
            std::numeric_limits<float>::quiet_NaN());
        expect(
            glm::length(character.position() - before) < 0.0001F,
            "invalid character deltas do not move the character");
    }

    {
        vcr::ThirdPersonCamera camera;
        camera.reset(-1.57079632679F);
        const glm::vec3 target{0.0F, 0.0F, 0.0F};
        const glm::vec3 initial_position = camera.position(target);
        expect(
            initial_position.y > 1.2F && initial_position.z > 0.0F &&
                camera.planar_forward().z < -0.99F,
            "third-person camera starts above and behind its target");
        const glm::vec3 initial_forward = camera.forward();
        camera.rotate(100.0F, 0.0F);
        expect(
            glm::length(camera.forward() - initial_forward) > 0.01F,
            "mouse X orbits the third-person camera");
        camera.rotate(0.0F, 100'000.0F);
        expect(
            camera.pitch() < 0.0F && camera.pitch() >= -1.3091F,
            "third-person camera pitch stays above the character and avoids a pole");
        const float yaw = camera.yaw();
        camera.rotate(std::numeric_limits<float>::quiet_NaN(), 0.0F);
        expect(
            camera.yaw() == yaw,
            "invalid mouse input does not change the third-person camera");

        vcr::ThirdPersonCamera invalid_config(vcr::ThirdPersonCameraConfig{
            .default_pitch_radians =
                std::numeric_limits<float>::quiet_NaN(),
        });
        invalid_config.reset(0.0F);
        expect(
            std::isfinite(invalid_config.pitch()),
            "invalid camera configuration falls back to a finite pitch");
    }

    if (failures != 0) {
        std::cerr << failures << " gameplay test(s) failed\n";
        return 1;
    }
    std::cout << "All gameplay tests passed\n";
    return 0;
}
