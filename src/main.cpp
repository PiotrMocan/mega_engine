#include "application.hpp"
#include "crash_log.hpp"

#include <exception>
#include <iostream>

int main(const int argc, char** argv) {
    vcr::install_crash_handlers("vulkan_cube", argc, argv);
    try {
        vcr::Application application;
        return application.run(argc, argv);
    } catch (const std::exception& error) {
        const auto log_path = vcr::write_crash_log("Fatal Exception", error.what());
        std::cerr << "fatal exception: " << error.what() << '\n';
        if (!log_path.empty()) {
            std::cerr << "crash log written to: " << log_path.string() << '\n';
        }
        return 1;
    } catch (...) {
        const auto log_path = vcr::write_crash_log(
            "Unknown Fatal Exception",
            "Non-std::exception thrown");
        std::cerr << "fatal exception: unknown non-std::exception\n";
        if (!log_path.empty()) {
            std::cerr << "crash log written to: " << log_path.string() << '\n';
        }
        return 1;
    }
}
