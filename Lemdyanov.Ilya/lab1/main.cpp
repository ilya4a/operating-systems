#include "Config.h"
#include "DirectoryMonitor.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class SyslogSession {
public:
    SyslogSession() {
        openlog("disk-monitor", LOG_PID | LOG_NDELAY, LOG_LOCAL0);
    }

    ~SyslogSession() {
        closelog();
    }

    SyslogSession(const SyslogSession&) = delete;
    SyslogSession& operator=(const SyslogSession&) = delete;
};

class PidFile {
public:
    PidFile() = default;

    ~PidFile() {
        release();
    }

    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

    bool acquire(const std::filesystem::path& path, std::string& error_message) {
        if (descriptor_ >= 0) {
            error_message = "PID file is already acquired by this object";
            return false;
        }
        if (path.empty()) {
            error_message = "PID-file path is empty";
            return false;
        }

        const int descriptor = open(path.c_str(),
                                    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                    0644);
        if (descriptor < 0) {
            error_message = system_error_message(
                "Cannot open PID file '" + path.string() + "'", errno);
            return false;
        }

        struct stat file_status{};
        if (fstat(descriptor, &file_status) < 0) {
            const int error_number = errno;
            error_message = system_error_message(
                "Cannot inspect PID file '" + path.string() + "'", error_number);
            close(descriptor);
            return false;
        }
        if (!S_ISREG(file_status.st_mode)) {
            error_message = "PID-file path '" + path.string() + "' is not a regular file";
            close(descriptor);
            return false;
        }

        int lock_result;
        do {
            lock_result = flock(descriptor, LOCK_EX | LOCK_NB);
        } while (lock_result < 0 && errno == EINTR);
        if (lock_result < 0) {
            const int error_number = errno;
            close(descriptor);
            if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
                error_message = "PID file '" + path.string() +
                                "' is locked by another running instance";
            } else {
                error_message = system_error_message(
                    "Cannot lock PID file '" + path.string() + "'", error_number);
            }
            return false;
        }

        // Do not write through an open descriptor if the path was replaced
        // between open() and flock().
        struct stat path_status{};
        if (lstat(path.c_str(), &path_status) < 0) {
            const int error_number = errno;
            error_message = system_error_message(
                "Cannot verify PID-file path '" + path.string() + "'", error_number);
            close(descriptor);
            return false;
        }
        if (path_status.st_dev != file_status.st_dev ||
            path_status.st_ino != file_status.st_ino ||
            !S_ISREG(path_status.st_mode)) {
            error_message = "PID-file path '" + path.string() +
                            "' changed while acquiring its lock";
            close(descriptor);
            return false;
        }

        const std::string contents = std::to_string(static_cast<long long>(getpid())) + "\n";
        if (ftruncate(descriptor, 0) < 0) {
            const int error_number = errno;
            error_message = system_error_message(
                "Cannot truncate PID file '" + path.string() + "'", error_number);
            close(descriptor);
            return false;
        }

        std::size_t offset = 0U;
        while (offset < contents.size()) {
            const ssize_t bytes_written = pwrite(
                descriptor, contents.data() + offset, contents.size() - offset,
                static_cast<off_t>(offset));
            if (bytes_written < 0 && errno == EINTR) {
                continue;
            }
            if (bytes_written <= 0) {
                const int error_number = bytes_written == 0 ? EIO : errno;
                error_message = system_error_message(
                    "Cannot write PID file '" + path.string() + "'", error_number);
                close(descriptor);
                return false;
            }
            offset += static_cast<std::size_t>(bytes_written);
        }

        if (fsync(descriptor) < 0) {
            const int error_number = errno;
            error_message = system_error_message(
                "Cannot synchronize PID file '" + path.string() + "'", error_number);
            close(descriptor);
            return false;
        }

        path_ = path;
        descriptor_ = descriptor;
        device_ = file_status.st_dev;
        inode_ = file_status.st_ino;
        return true;
    }

private:
    int descriptor_ = -1;
    std::filesystem::path path_;
    dev_t device_ = 0;
    ino_t inode_ = 0;

    static std::string system_error_message(const std::string& operation,
                                            int error_number) {
        return operation + ": " + std::strerror(error_number);
    }

    void release() noexcept {
        if (descriptor_ < 0) {
            return;
        }

        struct stat path_status{};
        if (lstat(path_.c_str(), &path_status) == 0) {
            if (path_status.st_dev == device_ && path_status.st_ino == inode_) {
                if (unlink(path_.c_str()) < 0 && errno != ENOENT) {
                    const int error_number = errno;
                    syslog(LOG_WARNING, "Cannot remove PID file '%s': %s",
                           path_.c_str(), std::strerror(error_number));
                }
            } else {
                syslog(LOG_WARNING,
                       "PID-file path '%s' no longer refers to the locked file; leaving it untouched",
                       path_.c_str());
            }
        } else if (errno != ENOENT) {
            const int error_number = errno;
            syslog(LOG_WARNING, "Cannot inspect PID-file path '%s' during cleanup: %s",
                   path_.c_str(), std::strerror(error_number));
        }

        if (close(descriptor_) < 0) {
            const int error_number = errno;
            syslog(LOG_WARNING, "Cannot close PID file '%s': %s",
                   path_.c_str(), std::strerror(error_number));
        }
        descriptor_ = -1;
    }
};

class SignalSource {
public:
    SignalSource() = default;

    ~SignalSource() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    SignalSource(const SignalSource&) = delete;
    SignalSource& operator=(const SignalSource&) = delete;

    bool initialize(std::string& error_message) {
        sigset_t signal_mask{};
        if (sigemptyset(&signal_mask) == -1) {
            error_message = system_error_message("sigemptyset failed");
            return false;
        }
        if (sigaddset(&signal_mask, SIGTERM) == -1 ||
            sigaddset(&signal_mask, SIGINT) == -1 ||
            sigaddset(&signal_mask, SIGHUP) == -1) {
            error_message = system_error_message("sigaddset failed");
            return false;
        }

        // Block these signals and read them synchronously from signalfd. This
        // avoids the check-then-poll race of an ordinary sig_atomic_t flag.
        if (sigprocmask(SIG_BLOCK, &signal_mask, nullptr) == -1) {
            error_message = system_error_message("sigprocmask failed");
            return false;
        }

        descriptor_ = signalfd(-1, &signal_mask, SFD_CLOEXEC | SFD_NONBLOCK);
        if (descriptor_ < 0) {
            error_message = system_error_message("signalfd failed");
            return false;
        }
        return true;
    }

    int descriptor() const noexcept {
        return descriptor_;
    }

    bool consume(bool& reload_requested,
                 int& termination_signal,
                 std::string& error_message) const {
        while (true) {
            signalfd_siginfo signal_info{};
            const ssize_t bytes_read = read(descriptor_, &signal_info, sizeof(signal_info));
            if (bytes_read == static_cast<ssize_t>(sizeof(signal_info))) {
                const int signal_number = static_cast<int>(signal_info.ssi_signo);
                if (signal_number == SIGHUP) {
                    reload_requested = true;
                } else if (signal_number == SIGTERM || signal_number == SIGINT) {
                    termination_signal = signal_number;
                }
                continue;
            }

            if (bytes_read < 0 && errno == EINTR) {
                continue;
            }
            if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            if (bytes_read < 0) {
                error_message = system_error_message("read from signalfd failed");
            } else {
                error_message = "Unexpected short read from signalfd";
            }
            return false;
        }
    }

private:
    int descriptor_ = -1;

    static std::string system_error_message(const std::string& operation) {
        const int error_number = errno;
        return operation + ": " + std::strerror(error_number);
    }
};

class WorkingDirectoryGuard {
public:
    WorkingDirectoryGuard() = default;

    ~WorkingDirectoryGuard() {
        if (active_) {
            std::error_code ignored_error;
            std::filesystem::current_path(previous_directory_, ignored_error);
        }
    }

    WorkingDirectoryGuard(const WorkingDirectoryGuard&) = delete;
    WorkingDirectoryGuard& operator=(const WorkingDirectoryGuard&) = delete;

    bool change_to(const std::filesystem::path& directory, std::string& error_message) {
        std::error_code filesystem_error;
        previous_directory_ = std::filesystem::current_path(filesystem_error);
        if (filesystem_error) {
            error_message = "Cannot determine current working directory: " +
                            filesystem_error.message();
            return false;
        }

        std::filesystem::current_path(directory, filesystem_error);
        if (filesystem_error) {
            error_message = "Cannot temporarily switch to configuration directory '" +
                            directory.string() + "': " + filesystem_error.message();
            return false;
        }
        active_ = true;
        return true;
    }

    bool restore(std::string& error_message) {
        if (!active_) {
            return true;
        }

        std::error_code filesystem_error;
        std::filesystem::current_path(previous_directory_, filesystem_error);
        if (filesystem_error) {
            error_message = "Cannot restore working directory: " +
                            filesystem_error.message();
            return false;
        }
        active_ = false;
        return true;
    }

private:
    std::filesystem::path previous_directory_;
    bool active_ = false;
};

bool load_config_relative_to_file(const std::filesystem::path& absolute_config_path,
                                  Config& config,
                                  std::string& error_message) {
    WorkingDirectoryGuard working_directory;
    if (!working_directory.change_to(absolute_config_path.parent_path(), error_message)) {
        return false;
    }

    const bool loaded = config.load(absolute_config_path, error_message);
    std::string restore_error;
    if (!working_directory.restore(restore_error)) {
        error_message = restore_error;
        return false;
    }
    return loaded;
}

void log_started_directories(const Config& config) {
    syslog(LOG_INFO, "Disk Monitor started successfully; watching %zu director%s",
           config.directories().size(),
           config.directories().size() == 1U ? "y" : "ies");
}

bool resolve_monitor_paths(const Config& config,
                           const std::filesystem::path& absolute_config_path,
                           std::vector<std::filesystem::path>& directories,
                           std::filesystem::path& event_log,
                           std::string& error_message) {
    namespace fs = std::filesystem;
    const fs::path config_directory = absolute_config_path.parent_path();

    const auto resolve = [&config_directory](const fs::path& path) {
        if (path.is_absolute()) {
            return path.lexically_normal();
        }
        return (config_directory / path).lexically_normal();
    };

    directories.clear();
    directories.reserve(config.directories().size());
    for (const auto& directory : config.directories()) {
        if (directory.empty()) {
            error_message = "An empty directory path was specified in the configuration";
            return false;
        }
        directories.push_back(resolve(directory));
    }

    if (config.event_log().empty()) {
        error_message = "An empty event-log path was specified in the configuration";
        return false;
    }
    event_log = resolve(config.event_log());
    return true;
}

bool resolve_pid_file_path(const Config& config,
                           const std::filesystem::path& absolute_config_path,
                           std::filesystem::path& pid_file,
                           std::string& error_message) {
    namespace fs = std::filesystem;
    if (config.pid_file().empty()) {
        error_message = "An empty PID-file path was specified in the configuration";
        return false;
    }

    const fs::path configured_path = config.pid_file();
    if (configured_path.is_absolute()) {
        pid_file = configured_path.lexically_normal();
    } else {
        pid_file = (absolute_config_path.parent_path() / configured_path).lexically_normal();
    }
    return true;
}

void reload_configuration(const std::filesystem::path& absolute_config_path,
                          const std::filesystem::path& active_pid_file_path,
                          Config& active_config,
                          DirectoryMonitor& monitor) {
    Config candidate_config;
    std::string error_message;
    if (!load_config_relative_to_file(absolute_config_path, candidate_config, error_message)) {
        syslog(LOG_ERR,
               "Configuration reload failed; keeping current configuration: %s",
               error_message.c_str());
        return;
    }

    std::filesystem::path candidate_pid_file_path;
    if (!resolve_pid_file_path(candidate_config, absolute_config_path,
                               candidate_pid_file_path, error_message)) {
        syslog(LOG_ERR,
               "Configuration reload failed; keeping current configuration: %s",
               error_message.c_str());
        return;
    }
    if (candidate_pid_file_path != active_pid_file_path) {
        syslog(LOG_ERR,
               "Configuration reload requested a different PID-file path; the PID-file path cannot be changed while running");
        return;
    }

    std::vector<std::filesystem::path> directories;
    std::filesystem::path event_log;
    if (!resolve_monitor_paths(candidate_config, absolute_config_path,
                               directories, event_log, error_message)) {
        syslog(LOG_ERR,
               "Configuration reload failed; keeping current configuration: %s",
               error_message.c_str());
        return;
    }

    if (!monitor.start(directories, event_log)) {
        syslog(LOG_ERR,
               "Configuration reload could not be applied; keeping current configuration and watches");
        return;
    }

    active_config = std::move(candidate_config);
    syslog(LOG_INFO, "Configuration reloaded successfully from %s",
           absolute_config_path.c_str());
    log_started_directories(active_config);
}

} // namespace

int main(int argc, char* argv[]) {
    SyslogSession syslog_session;
    syslog(LOG_INFO, "Disk Monitor starting");

    if (argc > 2) {
        const char* message = "Usage: disk-monitor [config-file]";
        std::cerr << message << '\n';
        syslog(LOG_ERR, "%s", message);
        return 1;
    }

    SignalSource signal_source;
    std::string error_message;
    if (!signal_source.initialize(error_message)) {
        std::cerr << "Cannot initialize signal handling: " << error_message << '\n';
        syslog(LOG_ERR, "Cannot initialize signal handling: %s", error_message.c_str());
        return 1;
    }

    namespace fs = std::filesystem;
    const fs::path config_argument = (argc == 2) ? argv[1] : "daemon.conf";
    std::error_code path_error;
    const fs::path absolute_config_path =
        fs::absolute(config_argument, path_error).lexically_normal();
    if (path_error) {
        std::cerr << "Cannot resolve configuration path: "
                  << path_error.message() << '\n';
        syslog(LOG_ERR, "Cannot resolve configuration path '%s': %s",
               config_argument.c_str(), path_error.message().c_str());
        return 1;
    }

    Config config;
    if (!load_config_relative_to_file(absolute_config_path, config, error_message)) {
        std::cerr << "Configuration error: " << error_message << '\n';
        syslog(LOG_ERR, "Configuration error: %s", error_message.c_str());
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }

    std::vector<fs::path> directories;
    fs::path event_log;
    if (!resolve_monitor_paths(config, absolute_config_path,
                               directories, event_log, error_message)) {
        std::cerr << "Configuration path error: " << error_message << '\n';
        syslog(LOG_ERR, "Configuration path error: %s", error_message.c_str());
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }

    fs::path pid_file_path;
    if (!resolve_pid_file_path(config, absolute_config_path,
                               pid_file_path, error_message)) {
        std::cerr << "PID-file path error: " << error_message << '\n';
        syslog(LOG_ERR, "PID-file path error: %s", error_message.c_str());
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }

    PidFile pid_file;
    if (!pid_file.acquire(pid_file_path, error_message)) {
        std::cerr << "Cannot acquire PID file: " << error_message << '\n';
        syslog(LOG_ERR, "Cannot acquire PID file: %s", error_message.c_str());
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }
    syslog(LOG_INFO, "Acquired PID-file lock at '%s' for PID %ld",
           pid_file_path.c_str(), static_cast<long>(getpid()));

    DirectoryMonitor monitor;
    if (!monitor.start(directories, event_log)) {
        std::cerr << "Failed to start directory monitor; see system log and event log\n";
        syslog(LOG_ERR, "Failed to start directory monitor");
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }

    log_started_directories(config);

    bool reload_requested = false;
    int termination_signal = 0;
    while (termination_signal == 0) {
        if (reload_requested) {
            // Clear before reloading. Signals arriving during the reload remain
            // queued on signalfd and will wake the next poll immediately.
            reload_requested = false;
            syslog(LOG_INFO, "SIGHUP received; reloading configuration");
            reload_configuration(absolute_config_path, pid_file_path, config, monitor);
            continue;
        }

        if (!monitor.process_events(signal_source.descriptor())) {
            syslog(LOG_ERR, "Disk Monitor stopped because of a monitoring error");
            break;
        }

        if (!signal_source.consume(reload_requested, termination_signal, error_message)) {
            syslog(LOG_ERR, "Cannot read signal descriptor: %s", error_message.c_str());
            break;
        }
    }

    monitor.stop();
    if (termination_signal != 0) {
        syslog(LOG_NOTICE, "Disk Monitor stopping after signal %d", termination_signal);
    } else {
        syslog(LOG_NOTICE, "Disk Monitor stopping after an internal error");
    }
    return termination_signal == 0 ? 1 : 0;
}
