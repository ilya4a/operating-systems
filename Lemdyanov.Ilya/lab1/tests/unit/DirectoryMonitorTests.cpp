#include "../../include/DirectoryMonitor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
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
                ("disk-monitor-dir-monitor-tests-" + std::to_string(getpid()) + "-" +
                 std::to_string(nonce));
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

class Pipe {
public:
    Pipe() {
        if (::pipe(descriptors_) != 0) {
            throw std::runtime_error("Test setup failed: pipe() failed");
        }
    }

    ~Pipe() {
        if (descriptors_[0] >= 0) ::close(descriptors_[0]);
        if (descriptors_[1] >= 0) ::close(descriptors_[1]);
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    int read_end() const noexcept { return descriptors_[0]; }
    int write_end() const noexcept { return descriptors_[1]; }

private:
    int descriptors_[2] = {-1, -1};
};

std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Test setup failed: could not read " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& contents = "test data\n") {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Test setup failed: could not create " + path.string());
    }
    output << contents;
    output.close();
    if (!output) {
        throw std::runtime_error("Test setup failed: could not write " + path.string());
    }
}

std::size_t occurrences(const std::string& text, const std::string& token) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos) {
        ++count;
        position += token.size();
    }
    return count;
}

void test_process_events_before_start_returns_false() {
    DirectoryMonitor monitor;
    CHECK(!monitor.process_events(-1));
}

void test_start_rejects_empty_directory_list() {
    TempDirectory temp;
    DirectoryMonitor monitor;
    CHECK(!monitor.start({}, temp.path() / "events.log"));
}

void test_start_rejects_relative_event_log() {
    TempDirectory temp;
    fs::create_directories(temp.path() / "watch");
    DirectoryMonitor monitor;
    CHECK(!monitor.start({temp.path() / "watch"}, "relative-events.log"));
}

void test_start_rejects_empty_event_log_path() {
    TempDirectory temp;
    fs::create_directories(temp.path() / "watch");
    DirectoryMonitor monitor;
    CHECK(!monitor.start({temp.path() / "watch"}, fs::path{}));
}

void test_start_rejects_relative_watch_directory() {
    TempDirectory temp;
    DirectoryMonitor monitor;
    CHECK(!monitor.start({"relative-watch"}, temp.path() / "events.log"));
}

void test_start_rejects_nonexistent_directory() {
    TempDirectory temp;
    DirectoryMonitor monitor;
    CHECK(!monitor.start({temp.path() / "missing"}, temp.path() / "events.log"));
}

void test_start_rejects_regular_file_as_directory() {
    TempDirectory temp;
    const auto not_directory = temp.path() / "not-a-directory";
    write_file(not_directory);
    DirectoryMonitor monitor;
    CHECK(!monitor.start({not_directory}, temp.path() / "events.log"));
}

void test_start_appends_to_log_and_stop_writes_markers() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch);
    write_file(log, "PREEXISTING_LOG_LINE\n");

    DirectoryMonitor monitor;
    CHECK(monitor.start({watch}, log));
    CHECK(read_text(log).find("PREEXISTING_LOG_LINE") != std::string::npos);
    CHECK(read_text(log).find("MONITOR_STARTED") != std::string::npos);

    monitor.stop();
    const auto contents = read_text(log);
    CHECK(contents.find("MONITOR_STOPPED") != std::string::npos);
}

void test_create_event_is_logged_with_full_path() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch);
    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch}, log));

    const auto created = watch / "created.txt";
    write_file(created);
    CHECK(monitor.process_events(interrupt_pipe.read_end()));

    const auto contents = read_text(log);
    CHECK(contents.find("CREATE") != std::string::npos);
    CHECK(contents.find(created.string()) != std::string::npos);
    monitor.stop();
}

void test_events_from_multiple_directories_are_logged() {
    TempDirectory temp;
    const auto watch_a = temp.path() / "watch-a";
    const auto watch_b = temp.path() / "watch-b";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch_a);
    fs::create_directories(watch_b);

    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch_a, watch_b}, log));
    const auto created_a = watch_a / "a.txt";
    const auto created_b = watch_b / "b.txt";
    write_file(created_a);
    write_file(created_b);
    CHECK(monitor.process_events(interrupt_pipe.read_end()));

    const auto contents = read_text(log);
    CHECK(contents.find(created_a.string()) != std::string::npos);
    CHECK(contents.find(created_b.string()) != std::string::npos);
    monitor.stop();
}

void test_delete_event_is_logged() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch);
    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch}, log));

    const auto target = watch / "to-delete.txt";
    write_file(target);
    CHECK(monitor.process_events(interrupt_pipe.read_end()));
    CHECK(fs::remove(target));
    CHECK(monitor.process_events(interrupt_pipe.read_end()));

    const auto contents = read_text(log);
    CHECK(contents.find("DELETE") != std::string::npos);
    CHECK(contents.find(target.string()) != std::string::npos);
    monitor.stop();
}

void test_successful_reconfiguration_switches_watches_and_log() {
    TempDirectory temp;
    const auto watch_a = temp.path() / "watch-a";
    const auto watch_b = temp.path() / "watch-b";
    const auto old_log = temp.path() / "old-events.log";
    const auto new_log = temp.path() / "new-events.log";
    fs::create_directories(watch_a);
    fs::create_directories(watch_b);

    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch_a}, old_log));
    CHECK(monitor.start({watch_b}, new_log));
    CHECK(read_text(old_log).find("MONITOR_RECONFIGURING") != std::string::npos);
    CHECK(read_text(new_log).find("MONITOR_STARTED") != std::string::npos);

    const auto old_target = watch_a / "ignored-after-reload.txt";
    const auto new_target = watch_b / "observed-after-reload.txt";
    write_file(old_target);
    write_file(new_target);
    CHECK(monitor.process_events(interrupt_pipe.read_end()));

    const auto new_contents = read_text(new_log);
    CHECK(new_contents.find(new_target.string()) != std::string::npos);
    CHECK(new_contents.find(old_target.string()) == std::string::npos);
    monitor.stop();
}

void test_failed_reconfiguration_preserves_existing_watch() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch);

    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch}, log));
    CHECK(!monitor.start({temp.path() / "missing"}, temp.path() / "failed-reload.log"));

    const auto created = watch / "still-watched.txt";
    write_file(created);
    CHECK(monitor.process_events(interrupt_pipe.read_end()));
    const auto contents = read_text(log);
    CHECK(contents.find("ERROR") != std::string::npos);
    CHECK(contents.find(created.string()) != std::string::npos);
    monitor.stop();
}

void test_interrupt_descriptor_wakes_process_events() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    fs::create_directories(watch);
    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch}, temp.path() / "events.log"));

    const char signal_byte = 'x';
    CHECK(::write(interrupt_pipe.write_end(), &signal_byte, sizeof(signal_byte)) ==
          static_cast<ssize_t>(sizeof(signal_byte)));
    CHECK(monitor.process_events(interrupt_pipe.read_end()));
    monitor.stop();
}

void test_invalid_interrupt_descriptor_stops_monitor() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch);
    DirectoryMonitor monitor;
    CHECK(monitor.start({watch}, log));

    CHECK(!monitor.process_events(-1));
    CHECK(read_text(log).find("ERROR Invalid interrupt descriptor") != std::string::npos);
    CHECK(read_text(log).find("MONITOR_STOPPED") != std::string::npos);
}

void test_stop_is_idempotent() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = temp.path() / "events.log";
    fs::create_directories(watch);
    DirectoryMonitor monitor;
    CHECK(monitor.start({watch}, log));
    monitor.stop();
    monitor.stop();
    CHECK(!monitor.process_events(-1));
    CHECK(occurrences(read_text(log), "MONITOR_STOPPED") == 1U);
}

void test_event_log_inside_watched_directory_does_not_log_itself() {
    TempDirectory temp;
    const auto watch = temp.path() / "watch";
    const auto log = watch / "events.log";
    fs::create_directories(watch);
    DirectoryMonitor monitor;
    Pipe interrupt_pipe;
    CHECK(monitor.start({watch}, log));

    // MONITOR_STARTED modifies the log after the watch is installed. Processing
    // that inotify event must not append a recursive event-log entry.
    CHECK(monitor.process_events(interrupt_pipe.read_end()));
    const auto contents = read_text(log);
    CHECK(occurrences(contents, "MONITOR_STARTED") == 1U);
    CHECK(contents.find("MODIFY " + log.string()) == std::string::npos);
    monitor.stop();
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"process_events before start returns false", test_process_events_before_start_returns_false},
        {"start rejects empty directory list", test_start_rejects_empty_directory_list},
        {"start rejects relative event log", test_start_rejects_relative_event_log},
        {"start rejects empty event-log path", test_start_rejects_empty_event_log_path},
        {"start rejects relative watch directory", test_start_rejects_relative_watch_directory},
        {"start rejects nonexistent directory", test_start_rejects_nonexistent_directory},
        {"start rejects regular file as directory", test_start_rejects_regular_file_as_directory},
        {"start appends to log and stop writes markers", test_start_appends_to_log_and_stop_writes_markers},
        {"create event is logged with full path", test_create_event_is_logged_with_full_path},
        {"multiple directories are monitored", test_events_from_multiple_directories_are_logged},
        {"delete event is logged", test_delete_event_is_logged},
        {"successful reconfiguration switches watches and log", test_successful_reconfiguration_switches_watches_and_log},
        {"failed reconfiguration preserves active watch", test_failed_reconfiguration_preserves_existing_watch},
        {"interrupt descriptor wakes process_events", test_interrupt_descriptor_wakes_process_events},
        {"invalid interrupt descriptor stops monitor", test_invalid_interrupt_descriptor_stops_monitor},
        {"stop is idempotent", test_stop_is_idempotent},
        {"event log inside watched directory does not log itself", test_event_log_inside_watched_directory_does_not_log_itself},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }

    std::cout << "\n" << (tests.size() - static_cast<std::size_t>(failures)) << "/"
              << tests.size() << " tests passed.\n";
    return failures == 0 ? 0 : 1;
}
