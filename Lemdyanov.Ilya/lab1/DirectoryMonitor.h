#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

class DirectoryMonitor {
public:
    DirectoryMonitor() = default;
    ~DirectoryMonitor();

    DirectoryMonitor(const DirectoryMonitor&) = delete;
    DirectoryMonitor& operator=(const DirectoryMonitor&) = delete;

    // Paths passed here must already be absolute. Resolve relative paths against
    // the configuration file's directory before calling start().
    bool start(const std::vector<std::filesystem::path>& directories,
               const std::filesystem::path& event_log);

    // Blocks until a filesystem event, a signal-source event, or an error.
    // Returns false if the monitor encounters a fatal runtime error.
    bool process_events(int interrupt_fd);

    void stop() noexcept;

private:
    int inotify_fd_ = -1;
    std::unordered_map<int, std::filesystem::path> watch_paths_;
    std::filesystem::path event_log_path_;
    std::ofstream event_log_stream_;

    void report_error(const std::string& message);
    void write_log(const std::string& message);
};
