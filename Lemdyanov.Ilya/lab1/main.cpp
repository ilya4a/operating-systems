#include "DirectoryMonitor.h"
#include "Config.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [config-file]\n";
        return 2;
    }

    const std::filesystem::path config_path = (argc == 2) ? argv[1] : "daemon.conf";
    std::error_code path_error;
    const auto absolute_config_path = std::filesystem::absolute(config_path, path_error);
    if (path_error) {
        std::cerr << "Cannot resolve configuration path: " << path_error.message() << '\n';
        return 1;
    }

    Config config;
    std::string error_message;
    if (!config.load(absolute_config_path, error_message)) {
        std::cerr << "Configuration error: " << error_message << '\n';
        return 1;
    }

    std::cout << "Disk Monitor started in foreground\n"
              << "Configuration: " << absolute_config_path << '\n'
              << "Event log: " << config.event_log() << '\n' << std::flush;

    DirectoryMonitor monitor;
    if (!monitor.start(config.directories(), config.event_log())) {
        return 1;
    }

    const int timeout_ms = config.interval_seconds() * 1000;
    while (true) {
        monitor.process_events(timeout_ms);
    }
}
