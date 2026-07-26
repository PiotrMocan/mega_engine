#include "editor_application.hpp"

#include <exception>
#include <iostream>

int main(const int argc, char** argv) {
    try {
        vcr::EditorApplication application;
        return application.run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fatal exception: " << error.what() << '\n';
        return 1;
    }
}
