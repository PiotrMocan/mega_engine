#include "project_manager_application.hpp"

#include <exception>
#include <iostream>

int main(const int argc, char** argv) {
    try {
        vcr::ProjectManagerApplication application;
        return application.run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fatal exception: " << error.what() << '\n';
        return 1;
    }
}
