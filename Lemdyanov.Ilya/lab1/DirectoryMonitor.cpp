#include "DirectoryMonitor.h"

bool DirectoryMonitor::start(const std::vector<std::string>& directories) {
    (void)directories;
    return false;
}

void DirectoryMonitor::process_events(int timeout_ms) {
    (void)timeout_ms;
}

void DirectoryMonitor::stop() noexcept {
    inotify_fd_ = -1;
}
