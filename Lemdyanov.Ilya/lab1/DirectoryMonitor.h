#ifndef DISK_MONITOR_DIRECTORYMONITOR_H
#define DISK_MONITOR_DIRECTORYMONITOR_H


#pragma once

#include <string>
#include <vector>

class DirectoryMonitor {
public:
    bool start(const std::vector<std::string>& directories);

    void process_events(int timeout_ms);

    void stop() noexcept;

private:
    int inotify_fd_ = -1;
};


#endif // DISK_MONITOR_DIRECTORYMONITOR_H
