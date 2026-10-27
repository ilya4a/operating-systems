#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

class Daemon final {
public:
    static Daemon& instance() noexcept;

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;

    // Forks, turns the child into a daemon, and makes the original parent exit.
    // Additional descriptors are preserved in the daemon (for example signalfd).
    // Returns in the daemon child; the original parent exits with the startup status.
    bool daemonize();
    bool daemonize(const std::vector<int>& additional_descriptors_to_keep);

    // Locks a stable sidecar file (<pid-path>.lock), then creates/updates the
    // PID file. The lock is inherited over fork() and the child PID is written
    // before daemonize() lets the parent exit.
    bool prepare_pid_file(const std::string& path);
    void release_pid_file() noexcept;

private:
    Daemon() = default;
    ~Daemon();

    int pid_file_fd_ = -1;
    int lock_file_fd_ = -1;
    std::filesystem::path pid_file_path_;
    dev_t pid_file_device_ = 0;
    ino_t pid_file_inode_ = 0;

    bool update_pid_after_fork();
    void release_pid_file_impl(bool report_errors) noexcept;
};
