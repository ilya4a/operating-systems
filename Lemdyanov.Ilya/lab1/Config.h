#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct Config {
    int interval_seconds = 5;
    std::vector<std::string> directories;
    std::string event_log;
    std::string pid_file;
};

bool load_config(const std::filesystem::path& path,
                 Config& config,
                 std::string& error_message);
std::string pid_file;
