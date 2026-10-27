#include "Config.h"
#include "Daemon.h"
#include "DirectoryMonitor.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/signalfd.h>
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

    // start() prepares new watches before replacing the active monitor. A failed
    // reconfiguration leaves the current watches and active Config unchanged.
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

    Daemon& daemon = Daemon::instance();
    if (!daemon.prepare_pid_file(pid_file_path.string())) {
        syslog(LOG_NOTICE, "Disk Monitor startup aborted while preparing PID file");
        return 1;
    }

    // SignalSource was initialized before fork(), so its blocked signal mask and
    // signalfd are inherited by the daemon child. Preserve that descriptor while
    // closing all other non-standard descriptors.
    if (!daemon.daemonize({signal_source.descriptor()})) {
        syslog(LOG_ERR, "Daemonization failed");
        daemon.release_pid_file();
        syslog(LOG_NOTICE, "Disk Monitor startup aborted during daemonization");
        return 1;
    }

    DirectoryMonitor monitor;
    if (!monitor.start(directories, event_log)) {
        std::cerr << "Failed to start directory monitor; see system log and event log\n";
        syslog(LOG_ERR, "Failed to start directory monitor");
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        daemon.release_pid_file();
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
    daemon.release_pid_file();
    return termination_signal == 0 ? 1 : 0;
}
