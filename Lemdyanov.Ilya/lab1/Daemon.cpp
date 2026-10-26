#include "Daemon.h"
Daemon& Daemon::instance() {
    static Daemon daemon;
    return daemon;
}

bool Daemon::daemonize() {
    return false;
}

bool Daemon::prepare_pid_file(const std::string& path) {
    (void)path;
    return false;
}

void Daemon::install_signal_handlers() {
    std::signal(SIGHUP, Daemon::signal_handler);
    std::signal(SIGTERM, Daemon::signal_handler);
}

void Daemon::signal_handler(int signal_number) noexcept {
    if (signal_number == SIGHUP) {
        reload_requested_ = 1;
    } else if (signal_number == SIGTERM) {
        stop_requested_ = 1;
    }
}
