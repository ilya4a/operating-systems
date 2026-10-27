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

    bool start(const std::vector<std::filesystem::path>& directories,
               const std::filesystem::path& event_log);
    void process_events(int timeout_ms);
    void stop() noexcept;

private:
    int inotify_fd_ = -1;
    std::unordered_map<int, std::filesystem::path> watch_paths_;
    std::filesystem::path event_log_path_;
    std::ofstream event_log_stream_;

    void write_log(const std::string& message);
};
