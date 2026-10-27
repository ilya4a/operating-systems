#include "../../include/Config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw std::runtime_error(std::string("CHECK failed: ") + #condition +          \
                                     " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        }                                                                                   \
    } while (false)

class TempDirectory {
public:
    TempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("disk-monitor-config-tests-" + std::to_string(nonce));
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

fs::path write_config(const fs::path& directory, const std::string& contents) {
    const fs::path path = directory / "daemon.conf";
    std::ofstream output(path);

    if (!output) {
        throw std::runtime_error(
            "Test setup failed: could not create " + path.string());
    }

    output << contents;
    output.close();

    if (!output) {
        throw std::runtime_error(
            "Test setup failed: could not write " + path.string());
    }

    return path;
}

void expect_load_failure(
    const std::string& contents,
    const std::string& expected_error) {

    TempDirectory temp;
    fs::create_directories(temp.path() / "watch");

    const auto config_path = write_config(temp.path(), contents);

    Config config;
    std::string error;

    CHECK(!config.load(config_path, error));
    CHECK(error.find(expected_error) != std::string::npos);
}

void test_valid_config_uses_defaults_and_resolves_relative_paths() {
    TempDirectory temp;
    fs::create_directories(temp.path() / "watch");

    const auto config_path = write_config(
        temp.path(),
        "# comment\n"
        "\n"
        "  directories = ./watch  \n");

    Config config;
    std::string error;

    CHECK(config.load(config_path, error));
    CHECK(error.empty());
    CHECK(config.directories().size() == 1);
    CHECK(config.directories().at(0) == temp.path() / "watch");
    CHECK(config.event_log() == temp.path() / "disk-monitor.log");
    CHECK(config.pid_file() == temp.path() / "disk-monitor.pid");
}

void test_custom_values_multiple_directories_and_whitespace() {
    TempDirectory temp;
    fs::create_directories(temp.path() / "watch-a");
    fs::create_directories(temp.path() / "watch-b");
    fs::create_directories(temp.path() / "logs");
    fs::create_directories(temp.path() / "run");

    const auto config_path = write_config(
        temp.path(),
        "  # leading whitespace before comment\n"
        "directories = ./watch-a ; ; ./watch-b ; \n"
        "event_log = ./logs/events.log\n"
        "pid_file = ./run/disk-monitor.pid\n");

    Config config;
    std::string error;

    CHECK(config.load(config_path, error));
    CHECK(config.directories().size() == 2);
    CHECK(config.directories().at(0) == temp.path() / "watch-a");
    CHECK(config.directories().at(1) == temp.path() / "watch-b");
    CHECK(config.event_log() == temp.path() / "logs/events.log");
    CHECK(config.pid_file() == temp.path() / "run/disk-monitor.pid");
}

void test_path_is_lexically_normalized() {
    TempDirectory temp;
    fs::create_directories(temp.path() / "watch");

    const auto config_path = write_config(
        temp.path(),
        "directories = ./unused/../watch\n"
        "event_log = logs/../events.log\n");

    Config config;
    std::string error;

    CHECK(config.load(config_path, error));
    CHECK(config.directories().at(0) == temp.path() / "watch");
    CHECK(config.event_log() == temp.path() / "events.log");
}

void test_absolute_paths_are_preserved() {
    TempDirectory temp;

    const auto external_watch = temp.path() / "absolute-watch";
    fs::create_directories(external_watch);

    const auto absolute_log = temp.path() / "absolute-events.log";
    const auto absolute_pid = temp.path() / "absolute.pid";

    const auto config_path = write_config(
        temp.path(),
        "directories = " + external_watch.string() + "\n" +
        "event_log = " + absolute_log.string() + "\n" +
        "pid_file = " + absolute_pid.string() + "\n");

    Config config;
    std::string error;

    CHECK(config.load(config_path, error));
    CHECK(config.directories().at(0) == external_watch);
    CHECK(config.event_log() == absolute_log);
    CHECK(config.pid_file() == absolute_pid);
}

void test_missing_configuration_file_fails() {
    TempDirectory temp;

    Config config;
    std::string error;

    CHECK(!config.load(temp.path() / "missing.conf", error));
    CHECK(error.find("Cannot open configuration file") != std::string::npos);
}

void test_line_without_equals_fails_with_line_number() {
    expect_load_failure(
        "# comment\n"
        "this is not key value\n"
        "directories=watch\n",
        "Expected key=value at line 2");
}

void test_empty_key_fails() {
    expect_load_failure(
        "=value\n"
        "directories=watch\n",
        "Empty key at line 1");
}

void test_unknown_key_fails() {
    expect_load_failure(
        "unknown_setting=value\n"
        "directories=watch\n",
        "Unknown configuration key at line 1: unknown_setting");
}

void test_directories_are_required_and_cannot_be_empty() {
    expect_load_failure(
        "",
        "Configuration must contain at least one directory");

    expect_load_failure(
        "directories=\n",
        "Configuration must contain at least one directory");

    expect_load_failure(
        "directories= ; ; \n",
        "Configuration must contain at least one directory");
}

void test_nonexistent_directory_fails() {
    expect_load_failure(
        "directories=does-not-exist\n",
        "Not a directory or directory does not exist");
}

void test_regular_file_cannot_be_used_as_directory() {
    TempDirectory temp;

    std::ofstream(temp.path() / "not-a-directory") << "file";

    const auto config_path = write_config(
        temp.path(),
        "directories=not-a-directory\n");

    Config config;
    std::string error;

    CHECK(!config.load(config_path, error));
    CHECK(error.find("Not a directory or directory does not exist")
          != std::string::npos);
}

void test_empty_event_log_fails() {
    expect_load_failure(
        "directories=watch\n"
        "event_log=   \n",
        "event_log cannot be empty");
}

void test_empty_pid_file_fails() {
    expect_load_failure(
        "directories=watch\n"
        "pid_file=   \n",
        "pid_file cannot be empty");
}

void test_failed_reload_does_not_overwrite_previous_config() {
    TempDirectory temp;
    fs::create_directories(temp.path() / "watch-good");

    const auto good_path = write_config(
        temp.path(),
        "directories=watch-good\n"
        "event_log=good.log\n"
        "pid_file=good.pid\n");

    Config config;
    std::string error;

    CHECK(config.load(good_path, error));

    const auto original_directories = config.directories();
    const auto original_event_log = config.event_log();
    const auto original_pid_file = config.pid_file();

    const auto bad_path = temp.path() / "bad.conf";

    {
        std::ofstream output(bad_path);
        output << "directories=missing-directory\n"
               << "event_log=bad.log\n";
    }

    CHECK(!config.load(bad_path, error));
    CHECK(config.directories() == original_directories);
    CHECK(config.event_log() == original_event_log);
    CHECK(config.pid_file() == original_pid_file);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()> >> tests = {
        {
            "valid config uses defaults and resolves relative paths",
            test_valid_config_uses_defaults_and_resolves_relative_paths
        },
        {
            "custom values, multiple directories and whitespace",
            test_custom_values_multiple_directories_and_whitespace
        },
        {
            "paths are lexically normalized",
            test_path_is_lexically_normalized
        },
        {
            "absolute paths are preserved",
            test_absolute_paths_are_preserved
        },
        {
            "missing configuration file fails",
            test_missing_configuration_file_fails
        },
        {
            "line without equals fails with line number",
            test_line_without_equals_fails_with_line_number
        },
        {
            "empty key fails",
            test_empty_key_fails
        },
        {
            "unknown key fails",
            test_unknown_key_fails
        },
        {
            "directories are required and cannot be empty",
            test_directories_are_required_and_cannot_be_empty
        },
        {
            "nonexistent directory fails",
            test_nonexistent_directory_fails
        },
        {
            "regular file cannot be used as directory",
            test_regular_file_cannot_be_used_as_directory
        },
        {
            "empty event_log fails",
            test_empty_event_log_fails
        },
        {
            "empty pid_file fails",
            test_empty_pid_file_fails
        },
        {
            "failed reload preserves previous config",
            test_failed_reload_does_not_overwrite_previous_config
        },
    };

    int failures = 0;

    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": "
                      << exception.what() << '\n';
        }
    }

    std::cout << "\n"
              << (tests.size() - static_cast<std::size_t>(failures))
              << "/" << tests.size() << " tests passed.\n";

    return failures == 0 ? 0 : 1;
}