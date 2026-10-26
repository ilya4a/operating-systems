#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {

    const std::filesystem::path config_path = (argc == 2) ? argv[1] : "daemon.conf";

    std::error_code error;
    const auto absolute_config_path = std::filesystem::absolute(config_path, error);

    if (error) {
        std::cerr << "Cannot resolve configuration path: " << error.message() << '\n';
        return 1;
    }

    std::cout << "Disk Monitor: skeleton\n" << "Configuration: " << absolute_config_path << '\n';
    return 0;
}
