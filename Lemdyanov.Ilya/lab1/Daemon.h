#pragma once

#include <csignal>
#include <string>

struct Config;

class Daemon {
public:
    static Daemon& instance();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    bool daemonize();
    bool prepare_pid_file(const std::string& path);

    void install_signal_handlers();
private:
    Daemon() = default;

    static void signal_handler(int signal_number) noexcept;
    static volatile std::sig_atomic_t reload_requested_;
    static volatile std::sig_atomic_t stop_requested_;
};

