#include "character_studio_application.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        vcr::CharacterStudioApplication app;
        return app.run(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "vulkan_character_studio fatal error: " << ex.what() << '\n';
        return 1;
    }
}
