#include "Config.h"
#include "DirectoryMonitor.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <syslog.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t termination_signal = 0;

void handle_termination_signal(int signal_number) noexcept {
    termination_signal = signal_number;
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

bool install_termination_handler(int signal_number, const char* signal_name) {
    struct sigaction action {};
    action.sa_handler = handle_termination_signal;
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

    if (!install_termination_handler(SIGTERM, "SIGTERM") ||
        !install_termination_handler(SIGINT, "SIGINT")) {
        std::cerr << "Cannot install termination signal handlers; see system log\n";
        return 1;
    }

    const std::filesystem::path config_path = (argc == 2) ? argv[1] : "daemon.conf";
    Config config;
    std::string error;
    if (!config.load(config_path, error)) {
        std::cerr << "Configuration error: " << error << '\n';
        syslog(LOG_ERR, "Configuration error: %s", error.c_str());
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

    syslog(LOG_INFO, "Disk Monitor started successfully; watching %zu director%s",
           config.directories().size(),
           config.directories().size() == 1U ? "y" : "ies");

    while (termination_signal == 0) {
        monitor.process_events(config.interval_seconds() * 1000);
    }

    monitor.stop();
    syslog(LOG_NOTICE, "Disk Monitor stopping after signal %d",
           static_cast<int>(termination_signal));
    return 0;
}
