#include "icon_tool_application.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        vcr::IconToolApplication app;
        return app.run(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
        return 1;
    }
}
