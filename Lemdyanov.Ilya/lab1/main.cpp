#include "Config.h"
#include "DirectoryMonitor.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <syslog.h>
#include <system_error>
#include <utility>

namespace {

volatile std::sig_atomic_t termination_signal = 0;
volatile std::sig_atomic_t reload_requested = 0;

void handle_termination_signal(int signal_number) noexcept {
    termination_signal = signal_number;
}

void handle_reload_signal(int) noexcept {
    reload_requested = 1;
}

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

using SignalHandler = void (*)(int);

bool install_signal_handler(int signal_number,
                            SignalHandler handler,
                            const char* signal_name) {
    struct sigaction action {};
    action.sa_handler = handler;
    action.sa_flags = 0;

    if (sigemptyset(&action.sa_mask) == -1) {
        const int error_number = errno;
        syslog(LOG_ERR, "Cannot initialize signal mask for %s: %s",
               signal_name, std::strerror(error_number));
        return false;
    }

    if (sigaction(signal_number, &action, nullptr) == -1) {
        const int error_number = errno;
        syslog(LOG_ERR, "Cannot install %s handler: %s",
               signal_name, std::strerror(error_number));
        return false;
    }

    return true;
}

void log_started_directories(const Config& config) {
    syslog(LOG_INFO, "Disk Monitor started successfully; watching %zu director%s",
           config.directories().size(),
           config.directories().size() == 1U ? "y" : "ies");
}

void reload_configuration(const std::filesystem::path& absolute_config_path,
                          Config& active_config,
                          DirectoryMonitor& monitor) {
    Config candidate_config;
    std::string error_message;

    if (!candidate_config.load(absolute_config_path, error_message)) {
        syslog(LOG_ERR, "Configuration reload failed; keeping current configuration: %s",
               error_message.c_str());
        return;
    }

    // DirectoryMonitor::start() prepares the replacement watches before it
    // discards the current ones. If preparation fails, the old monitor remains active.
    if (!monitor.start(candidate_config.directories(), candidate_config.event_log())) {
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

    if (!install_signal_handler(SIGTERM, handle_termination_signal, "SIGTERM") ||
        !install_signal_handler(SIGINT, handle_termination_signal, "SIGINT") ||
        !install_signal_handler(SIGHUP, handle_reload_signal, "SIGHUP")) {
        std::cerr << "Cannot install signal handlers; see system log\n";
        return 1;
    }

    const std::filesystem::path config_argument =
        (argc == 2) ? argv[1] : "daemon.conf";
    std::error_code path_error;
    const std::filesystem::path absolute_config_path =
        std::filesystem::absolute(config_argument, path_error).lexically_normal();
    if (path_error) {
        std::cerr << "Cannot resolve configuration path: "
                  << path_error.message() << '\n';
        syslog(LOG_ERR, "Cannot resolve configuration path '%s': %s",
               config_argument.c_str(), path_error.message().c_str());
        return 1;
    }

    Config config;
    std::string error_message;
    if (!config.load(absolute_config_path, error_message)) {
        std::cerr << "Configuration error: " << error_message << '\n';
        syslog(LOG_ERR, "Configuration error: %s", error_message.c_str());
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }

    DirectoryMonitor monitor;
    if (!monitor.start(config.directories(), config.event_log())) {
        std::cerr << "Failed to start directory monitor; see error output and event log\n";
        syslog(LOG_ERR, "Failed to start directory monitor");
        syslog(LOG_NOTICE, "Disk Monitor startup aborted");
        return 1;
    }

    log_started_directories(config);

    while (termination_signal == 0) {
        monitor.process_events(config.interval_seconds() * 1000);

        if (termination_signal != 0) {
            break;
        }

        if (reload_requested != 0) {
            // Clear the flag before reloading. If another SIGHUP arrives while
            // loading, its handler sets the flag again and it will be handled
            // by the next iteration.
            reload_requested = 0;
            syslog(LOG_INFO, "SIGHUP received; reloading configuration");
            reload_configuration(absolute_config_path, config, monitor);
        }
    }

    monitor.stop();
    syslog(LOG_NOTICE, "Disk Monitor stopping after signal %d",
           static_cast<int>(termination_signal));
    return 0;
}