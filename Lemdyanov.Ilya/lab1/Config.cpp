#include "Config.h"

#include <charconv>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::vector<std::string> split_directories(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0U;

    while (start <= value.size()) {
        const auto separator = value.find(';', start);
        const auto end = (separator == std::string::npos) ? value.size() : separator;
        const auto item = trim(value.substr(start, end - start));
        if (!item.empty()) {
            result.push_back(item);
        }

        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1U;
    }

    return result;
}

bool validate_output_path(const std::filesystem::path& path,
                          const std::string& setting_name,
                          std::string& error_message) {
    namespace fs = std::filesystem;

    std::error_code error;
    const auto parent = path.parent_path();
    if (parent.empty() || !fs::is_directory(parent, error)) {
        error_message = "Parent directory for '" + setting_name + "' does not exist or "
                        "is not accessible: " + (parent.empty() ? std::string(".") : parent.string());
        if (error) {
            error_message += " (" + error.message() + ")";
        }
        return false;
    }

    error.clear();
    if (fs::exists(path, error)) {
        if (error) {
            error_message = "Cannot inspect '" + setting_name + "': " + error.message();
            return false;
        }
        error.clear();
        if (fs::is_directory(path, error)) {
            error_message = "'" + setting_name + "' points to a directory, not a file: " +
                            path.string();
            return false;
        }
        if (error) {
            error_message = "Cannot inspect '" + setting_name + "': " + error.message();
            return false;
        }
    } else if (error) {
        error_message = "Cannot inspect '" + setting_name + "': " + error.message();
        return false;
    }

    return true;
}

} // namespace

bool Config::load(const std::filesystem::path& path, std::string& error_message) {
    namespace fs = std::filesystem;
    error_message.clear();

    std::error_code filesystem_error;
    const fs::path absolute_config_path = fs::absolute(path, filesystem_error).lexically_normal();
    if (filesystem_error) {
        error_message = "Cannot resolve configuration path '" + path.string() + "': " +
                        filesystem_error.message();
        return false;
    }

    filesystem_error.clear();
    if (!fs::is_regular_file(absolute_config_path, filesystem_error)) {
        error_message = "Configuration file does not exist or is not a regular file: " +
                        absolute_config_path.string();
        if (filesystem_error) {
            error_message += " (" + filesystem_error.message() + ")";
        }
        return false;
    }

    std::ifstream input(absolute_config_path);
    if (!input.is_open()) {
        error_message = "Cannot open configuration file: " + absolute_config_path.string();
        return false;
    }

    const fs::path base_directory = absolute_config_path.parent_path();

    int interval_seconds = 5;
    std::vector<std::string> directory_values;
    std::string event_log_value = "disk-monitor.log";
    std::string pid_file_value = "disk-monitor.pid";
    bool directories_were_set = false;
    std::size_t setting_count = 0U;
    std::unordered_set<std::string> seen_keys;

    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        const auto cleaned_line = trim(line);
        if (cleaned_line.empty() || cleaned_line.front() == '#') {
            continue;
        }

        const auto equals = cleaned_line.find('=');
        if (equals == std::string::npos) {
            error_message = "Expected key=value at line " + std::to_string(line_number);
            return false;
        }

        const auto key = trim(cleaned_line.substr(0U, equals));
        const auto value = trim(cleaned_line.substr(equals + 1U));
        if (key.empty()) {
            error_message = "Empty configuration key at line " + std::to_string(line_number);
            return false;
        }
        if (!seen_keys.insert(key).second) {
            error_message = "Duplicate configuration key '" + key + "' at line " +
                            std::to_string(line_number);
            return false;
        }
        ++setting_count;

        if (key == "interval_seconds") {
            int parsed_interval = 0;
            const auto* begin = value.data();
            const auto* end = begin + value.size();
            const auto parse_result = std::from_chars(begin, end, parsed_interval);
            if (value.empty() || parse_result.ec != std::errc{} || parse_result.ptr != end ||
                parsed_interval < 1 || parsed_interval > 86400) {
                error_message = "'interval_seconds' must be an integer from 1 to 86400 "
                                "(line " + std::to_string(line_number) + ")";
                return false;
            }
            interval_seconds = parsed_interval;
        } else if (key == "directories") {
            directory_values = split_directories(value);
            directories_were_set = true;
        } else if (key == "event_log") {
            if (value.empty()) {
                error_message = "'event_log' cannot be empty (line " +
                                std::to_string(line_number) + ")";
                return false;
            }
            event_log_value = value;
        } else if (key == "pid_file") {
            if (value.empty()) {
                error_message = "'pid_file' cannot be empty (line " +
                                std::to_string(line_number) + ")";
                return false;
            }
            pid_file_value = value;
        } else {
            error_message = "Unknown configuration key '" + key + "' at line " +
                            std::to_string(line_number);
            return false;
        }
    }

    if (input.bad()) {
        error_message = "I/O error while reading configuration file: " +
                        absolute_config_path.string();
        return false;
    }
    if (setting_count == 0U) {
        error_message = "Configuration file is empty or contains only comments: " +
                        absolute_config_path.string();
        return false;
    }
    if (!directories_were_set) {
        error_message = "Missing required configuration key 'directories'";
        return false;
    }
    if (directory_values.empty()) {
        error_message = "'directories' must contain at least one directory";
        return false;
    }

    const auto resolve_path = [&base_directory](const std::string& value) {
        fs::path resolved(value);
        if (resolved.is_relative()) {
            resolved = base_directory / resolved;
        }
        return resolved.lexically_normal();
    };

    std::vector<fs::path> directories;
    directories.reserve(directory_values.size());
    for (const auto& directory_value : directory_values) {
        const auto directory = resolve_path(directory_value);
        filesystem_error.clear();
        if (!fs::is_directory(directory, filesystem_error)) {
            error_message = "Configured directory does not exist or is not a directory: " +
                            directory.string();
            if (filesystem_error) {
                error_message += " (" + filesystem_error.message() + ")";
            }
            return false;
        }
        directories.push_back(directory);
    }

    const auto event_log = resolve_path(event_log_value);
    if (!validate_output_path(event_log, "event_log", error_message)) {
        return false;
    }

    const auto pid_file = resolve_path(pid_file_value);
    if (!validate_output_path(pid_file, "pid_file", error_message)) {
        return false;
    }

    if (event_log == pid_file) {
        error_message = "'event_log' and 'pid_file' must refer to different files";
        return false;
    }

    interval_seconds_ = interval_seconds;
    directories_ = std::move(directories);
    event_log_ = event_log;
    pid_file_ = pid_file;
    return true;
}

int Config::interval_seconds() const noexcept {
    return interval_seconds_;
}

const std::vector<std::filesystem::path>& Config::directories() const noexcept {
    return directories_;
}

const std::filesystem::path& Config::event_log() const noexcept {
    return event_log_;
}

const std::filesystem::path& Config::pid_file() const noexcept {
    return pid_file_;
}
std::string trim(const std::string& value) {
