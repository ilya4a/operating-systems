#include "Daemon.h"

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <fcntl.h>
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
                ("disk-monitor-daemon-tests-" + std::to_string(getpid()) + "-" +
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

class DaemonCleanup {
public:
    DaemonCleanup() : daemon_(Daemon::instance()) { daemon_.release_pid_file(); }
    ~DaemonCleanup() { daemon_.release_pid_file(); }

    Daemon& get() noexcept { return daemon_; }

private:
    Daemon& daemon_;
};

std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Test setup failed: could not read " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& contents) {
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

fs::path pid_path(const TempDirectory& temp) {
    return temp.path() / "disk-monitor.pid";
}

void test_empty_pid_path_is_rejected() {
    DaemonCleanup cleanup;
    CHECK(!cleanup.get().prepare_pid_file(""));
}

void test_relative_pid_path_is_rejected() {
    DaemonCleanup cleanup;
    CHECK(!cleanup.get().prepare_pid_file("relative/disk-monitor.pid"));
}

void test_prepare_creates_pid_and_lock_files() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = pid_path(temp);

    CHECK(cleanup.get().prepare_pid_file(pid.string()));
    CHECK(fs::is_regular_file(pid));
    CHECK(fs::is_regular_file(pid.string() + ".lock"));
    CHECK(read_text(pid) == std::to_string(static_cast<long long>(getpid())) + "\n");
}

void test_existing_pid_file_is_truncated_and_rewritten() {
    TempDirectory temp;
    const auto pid = pid_path(temp);
    write_file(pid, "this is a stale PID value with extra trailing text\n");

    DaemonCleanup cleanup;
    CHECK(cleanup.get().prepare_pid_file(pid.string()));
    CHECK(read_text(pid) == std::to_string(static_cast<long long>(getpid())) + "\n");
}

void test_second_prepare_on_same_instance_is_rejected() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto first_pid = pid_path(temp);
    const auto second_pid = temp.path() / "other.pid";

    CHECK(cleanup.get().prepare_pid_file(first_pid.string()));
    CHECK(!cleanup.get().prepare_pid_file(second_pid.string()));
    CHECK(fs::exists(first_pid));
    CHECK(!fs::exists(second_pid));
}

void test_an_independent_file_description_cannot_take_lock() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = pid_path(temp);
    const fs::path lock = pid.string() + ".lock";
    CHECK(cleanup.get().prepare_pid_file(pid.string()));

    const int descriptor = ::open(lock.c_str(), O_RDWR);
    CHECK(descriptor >= 0);
    errno = 0;
    const int result = ::flock(descriptor, LOCK_EX | LOCK_NB);
    const int lock_error = errno;
    ::close(descriptor);
    CHECK(result == -1);
    CHECK(lock_error == EWOULDBLOCK || lock_error == EAGAIN);
}

void test_release_removes_pid_but_keeps_lock_file() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = pid_path(temp);
    const fs::path lock = pid.string() + ".lock";

    CHECK(cleanup.get().prepare_pid_file(pid.string()));
    cleanup.get().release_pid_file();
    CHECK(!fs::exists(pid));
    CHECK(fs::is_regular_file(lock));
}

void test_prepare_succeeds_again_after_release() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = pid_path(temp);

    CHECK(cleanup.get().prepare_pid_file(pid.string()));
    cleanup.get().release_pid_file();
    CHECK(cleanup.get().prepare_pid_file(pid.string()));
    CHECK(read_text(pid) == std::to_string(static_cast<long long>(getpid())) + "\n");
}

void test_symlink_pid_path_is_rejected_without_modifying_target() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto target = temp.path() / "real-file.txt";
    const auto pid = pid_path(temp);
    write_file(target, "do not overwrite\n");
    fs::create_symlink(target, pid);

    CHECK(!cleanup.get().prepare_pid_file(pid.string()));
    CHECK(fs::is_symlink(pid));
    CHECK(read_text(target) == "do not overwrite\n");
}

void test_symlink_lock_path_is_rejected() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto target = temp.path() / "lock-target.txt";
    const auto pid = pid_path(temp);
    const fs::path lock = pid.string() + ".lock";
    write_file(target, "lock target\n");
    fs::create_symlink(target, lock);

    CHECK(!cleanup.get().prepare_pid_file(pid.string()));
    CHECK(fs::is_symlink(lock));
    CHECK(read_text(target) == "lock target\n");
    CHECK(!fs::exists(pid));
}

void test_path_with_missing_parent_is_rejected() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = temp.path() / "missing-parent" / "disk-monitor.pid";
    CHECK(!cleanup.get().prepare_pid_file(pid.string()));
    CHECK(!fs::exists(pid));
}

void test_directory_cannot_be_used_as_pid_file() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = temp.path() / "pid-directory";
    fs::create_directory(pid);
    CHECK(!cleanup.get().prepare_pid_file(pid.string()));
    CHECK(fs::is_directory(pid));
}

void test_release_does_not_delete_replacement_pid_path() {
    TempDirectory temp;
    DaemonCleanup cleanup;
    const auto pid = pid_path(temp);
    const auto moved_aside = temp.path() / "original.pid";
    CHECK(cleanup.get().prepare_pid_file(pid.string()));

    fs::rename(pid, moved_aside);
    write_file(pid, "replacement file must survive\n");
    cleanup.get().release_pid_file();

    CHECK(fs::exists(pid));
    CHECK(read_text(pid) == "replacement file must survive\n");
    CHECK(fs::exists(moved_aside));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"empty PID path is rejected", test_empty_pid_path_is_rejected},
        {"relative PID path is rejected", test_relative_pid_path_is_rejected},
        {"prepare creates PID and lock files", test_prepare_creates_pid_and_lock_files},
        {"existing PID file is truncated and rewritten", test_existing_pid_file_is_truncated_and_rewritten},
        {"second prepare on same instance is rejected", test_second_prepare_on_same_instance_is_rejected},
        {"independent file description cannot take lock", test_an_independent_file_description_cannot_take_lock},
        {"release removes PID but keeps lock file", test_release_removes_pid_but_keeps_lock_file},
        {"prepare succeeds after release", test_prepare_succeeds_again_after_release},
        {"symlink PID path is rejected", test_symlink_pid_path_is_rejected_without_modifying_target},
        {"symlink lock path is rejected", test_symlink_lock_path_is_rejected},
        {"missing parent path is rejected", test_path_with_missing_parent_is_rejected},
        {"directory cannot be used as PID file", test_directory_cannot_be_used_as_pid_file},
        {"release preserves replacement PID path", test_release_does_not_delete_replacement_pid_path},
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
