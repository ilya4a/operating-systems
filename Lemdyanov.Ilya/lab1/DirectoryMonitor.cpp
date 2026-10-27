#include "DirectoryMonitor.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/inotify.h>
#include <unistd.h>
#include <vector>

#include "Daemon.h"


volatile std::sig_atomic_t Daemon::reload_requested_ = 0;
volatile std::sig_atomic_t Daemon::stop_requested_ = 0;


namespace {

std::string describe_event(std::uint32_t mask) {
    std::vector<std::string> names;

    if ((mask & IN_ACCESS) != 0U) names.emplace_back("ACCESS");
    if ((mask & IN_ATTRIB) != 0U) names.emplace_back("ATTRIB");
    if ((mask & IN_CLOSE_WRITE) != 0U) names.emplace_back("CLOSE_WRITE");
    if ((mask & IN_CLOSE_NOWRITE) != 0U) names.emplace_back("CLOSE_NOWRITE");
    if ((mask & IN_CREATE) != 0U) names.emplace_back("CREATE");
    if ((mask & IN_DELETE) != 0U) names.emplace_back("DELETE");
    if ((mask & IN_DELETE_SELF) != 0U) names.emplace_back("DELETE_SELF");
    if ((mask & IN_MODIFY) != 0U) names.emplace_back("MODIFY");
    if ((mask & IN_MOVE_SELF) != 0U) names.emplace_back("MOVE_SELF");
    if ((mask & IN_MOVED_FROM) != 0U) names.emplace_back("MOVED_FROM");
    if ((mask & IN_MOVED_TO) != 0U) names.emplace_back("MOVED_TO");
    if ((mask & IN_OPEN) != 0U) names.emplace_back("OPEN");
    if ((mask & IN_IGNORED) != 0U) names.emplace_back("IGNORED");
    if ((mask & IN_Q_OVERFLOW) != 0U) names.emplace_back("QUEUE_OVERFLOW");
    if ((mask & IN_UNMOUNT) != 0U) names.emplace_back("UNMOUNT");

    if (names.empty()) {
        return "UNKNOWN_EVENT";
    }

    std::ostringstream result;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0U) {
            result << '|';
        }
        result << names[i];
    }

    if ((mask & IN_ISDIR) != 0U) {
        result << "|IS_DIR";
    }
    return result.str();
}

} // namespace

DirectoryMonitor::~DirectoryMonitor() {
    stop();
}

bool DirectoryMonitor::start(const std::vector<std::filesystem::path>& directories,
                              const std::filesystem::path& event_log) {
    stop();

    event_log_path_ = event_log.lexically_normal();
    event_log_stream_.open(event_log_path_, std::ios::out | std::ios::app);
    if (!event_log_stream_) {
        std::cerr << "Cannot open event log: " << event_log_path_ << '\n';
        return false;
    }

    inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        std::cerr << "inotify_init1 failed: " << std::strerror(errno) << '\n';
        stop();
        return false;
    }

    constexpr std::uint32_t watch_mask = IN_ACCESS | IN_ATTRIB | IN_CLOSE_WRITE |
        IN_CLOSE_NOWRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY |
        IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_OPEN;

    for (const auto& directory : directories) {
        const auto normalized_directory = directory.lexically_normal();
        const int watch_descriptor = inotify_add_watch(
            inotify_fd_, normalized_directory.c_str(), watch_mask);
        if (watch_descriptor < 0) {
            std::cerr << "Cannot watch directory " << normalized_directory << ": "
                      << std::strerror(errno) << '\n';
            stop();
            return false;
        }
        watch_paths_[watch_descriptor] = normalized_directory;
    }

    write_log("MONITOR_STARTED");
    return true;
}

void DirectoryMonitor::process_events(int timeout_ms) {
    if (inotify_fd_ < 0) {
        return;
    }

    pollfd descriptor{};
    descriptor.fd = inotify_fd_;
    descriptor.events = POLLIN;

    const int poll_result = poll(&descriptor, 1, timeout_ms);
    if (poll_result < 0) {
        if (errno == EINTR) {
            return;
        }
        const std::string message = "poll failed: " + std::string(std::strerror(errno));
        std::cerr << message << '\n';
        write_log("ERROR " + message);
        return;
    }
    if (poll_result == 0) {
        return;
    }
    if ((descriptor.revents & POLLIN) == 0) {
        const std::string message = "poll returned unexpected event flags";
        std::cerr << message << '\n';
        write_log("ERROR " + message);
        return;
    }

    alignas(inotify_event) std::array<char, 64U * 1024U> buffer{};
    while (true) {
        const auto bytes_read = read(inotify_fd_, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            const std::string message = "read failed: " + std::string(std::strerror(errno));
            std::cerr << message << '\n';
            write_log("ERROR " + message);
            break;
        }
        if (bytes_read == 0) {
            break;
        }

        const auto available = static_cast<std::size_t>(bytes_read);
        std::size_t offset = 0;
        while (offset + sizeof(inotify_event) <= available) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
            const auto event_size = sizeof(inotify_event) + event->len;
            if (offset + event_size > available) {
                break;
            }

            std::filesystem::path target;
            if ((event->mask & IN_Q_OVERFLOW) != 0U) {
                target = "[inotify-queue]";
            } else {
                const auto watch = watch_paths_.find(event->wd);
                if (watch == watch_paths_.end()) {
                    target = "[unknown-watch]";
                } else {
                    target = watch->second;
                    if (event->len > 0U && event->name[0] != '\0') {
                        target /= event->name;
                    }
                }
            }
            target = target.lexically_normal();

            // Do not log writes to the log file itself if it is inside a watched directory.
            if ((event->mask & IN_Q_OVERFLOW) != 0U || target != event_log_path_) {
                write_log(describe_event(event->mask) + " " + target.string());
            }

            offset += event_size;
        }
    }
}

void DirectoryMonitor::stop() noexcept {
    if (inotify_fd_ >= 0) {
        if (event_log_stream_.is_open()) {
            write_log("MONITOR_STOPPED");
        }
        close(inotify_fd_);
        inotify_fd_ = -1;
    }

    watch_paths_.clear();
    if (event_log_stream_.is_open()) {
        event_log_stream_.close();
    }
}

void DirectoryMonitor::write_log(const std::string& message) {
    if (!event_log_stream_) {
        return;
    }

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local_time{};
    if (localtime_r(&now, &local_time) == nullptr) {
        event_log_stream_ << "[time-unavailable] ";
    } else {
        event_log_stream_ << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << ' ';
    }
    event_log_stream_ << message << '\n';
    event_log_stream_.flush();
}
