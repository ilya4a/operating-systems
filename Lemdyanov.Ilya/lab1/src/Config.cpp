#include "../include/Config.h"

#include <charconv>
#include <fstream>
#include <system_error>
#include <utility>

namespace {

    std::string trim(const std::string &value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }

        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::vector<std::string> split_directories(const std::string &value) {
        std::vector<std::string> result;
        std::size_t start = 0;

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
            start = separator + 1;
        }

        return result;
    }

} // namespace

bool Config::load(const std::filesystem::path &path, std::string &error_message) {
    namespace fs = std::filesystem;

    std::ifstream input(path);
    if (!input) {
        error_message = "Cannot open configuration file: " + path.string();
        return false;
    }

    std::error_code path_error;
    fs::path base_directory = path.parent_path();
    if (base_directory.empty()) {
        base_directory = fs::current_path(path_error);
        if (path_error) {
            error_message = "Cannot determine current directory: " + path_error.message();
            return false;
        }
    }

    std::vector<std::string> directory_values;
    std::string event_log_value = "disk-monitor.log";
    std::string pid_file_value = "disk-monitor.pid";
    bool directories_were_set = false;

    std::string line;
    std::size_t line_number = 0;
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

        const auto key = trim(cleaned_line.substr(0, equals));
        const auto value = trim(cleaned_line.substr(equals + 1));
        if (key.empty()) {
            error_message = "Empty key at line " + std::to_string(line_number);
            return false;
        }

        if (key == "directories") {
            directory_values = split_directories(value);
            directories_were_set = true;
        } else if (key == "event_log") {
            if (value.empty()) {
                error_message = "event_log cannot be empty";
                return false;
            }
            event_log_value = value;
        } else if (key == "pid_file") {
            if (value.empty()) {
                error_message = "pid_file cannot be empty";
                return false;
            }
            pid_file_value = value;
        } else {
            error_message = "Unknown configuration key at line " + std::to_string(line_number) + ": " + key;
            return false;
        }
    }

    if (!input.eof() && input.fail()) {
        error_message = "Error while reading configuration file: " + path.string();
        return false;
    }

    if (!directories_were_set || directory_values.empty()) {
        error_message = "Configuration must contain at least one directory";
        return false;
    }

    const auto resolve_path = [&base_directory](const std::string &value) {
        fs::path result(value);
        if (result.is_relative()) {
            result = base_directory / result;
        }
        return result.lexically_normal();
    };

    std::vector<fs::path> directories;
    directories.reserve(directory_values.size());
    for (const auto &directory_value : directory_values) {
        const auto directory = resolve_path(directory_value);
        std::error_code directory_error;
        if (!fs::is_directory(directory, directory_error)) {
            error_message = "Not a directory or directory does not exist: " + directory.string();
            if (directory_error) {
                error_message += " (" + directory_error.message() + ")";
            }
            return false;
        }
        directories.push_back(directory);
    }

    directories_ = std::move(directories);
    event_log_ = resolve_path(event_log_value);
    pid_file_ = resolve_path(pid_file_value);
    return true;
}


const std::vector<std::filesystem::path> &Config::directories() const noexcept {
    return directories_;
}

const std::filesystem::path &Config::event_log() const noexcept {
    return event_log_;
}

const std::filesystem::path &Config::pid_file() const noexcept {
    return pid_file_;
}
