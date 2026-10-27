#include "DirectoryMonitor.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <vector>

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
    if ((mask & IN_UNMOUNT) != 0U) names.emplace_back("UNMOUNT");
    if ((mask & IN_Q_OVERFLOW) != 0U) names.emplace_back("QUEUE_OVERFLOW");

    if (names.empty()) {
        names.emplace_back("UNKNOWN_EVENT");
    }

    std::ostringstream result;
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (index != 0U) {
            result << '|';
        }
        result << names[index];
    }
    if ((mask & IN_ISDIR) != 0U) {
        result << "|IS_DIR";
    }
    return result.str();
}

std::string system_error_message(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

} // namespace

DirectoryMonitor::~DirectoryMonitor() {
    stop();
}

bool DirectoryMonitor::start(const std::vector<std::filesystem::path>& directories,
                             const std::filesystem::path& event_log) {
    namespace fs = std::filesystem;

    std::error_code filesystem_error;
    const fs::path new_event_log_path =
        fs::absolute(event_log, filesystem_error).lexically_normal();
    if (filesystem_error) {
        const auto message = "Cannot resolve event-log path '" + event_log.string() +
                             "': " + filesystem_error.message();
        std::cerr << message << '\n';
        if (event_log_stream_.is_open()) {
            write_log("ERROR " + message);
        }
        return false;
    }

    std::ofstream new_event_log_stream;
    new_event_log_stream.open(new_event_log_path, std::ios::out | std::ios::app);
    if (!new_event_log_stream.is_open()) {
        const auto message = "Cannot open event log '" + new_event_log_path.string() + "'";
        std::cerr << message << '\n';
        if (event_log_stream_.is_open()) {
            write_log("ERROR " + message);
        }
        return false;
    }

    const auto report_error = [this](const std::string& message) {
        std::cerr << message << '\n';
        if (event_log_stream_.is_open()) {
            write_log("ERROR " + message);
        }
    };

    const int new_inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (new_inotify_fd < 0) {
        report_error(system_error_message("inotify_init1 failed"));
        return false;
    }

    const auto cleanup_new_monitor = [&new_inotify_fd]() {
        close(new_inotify_fd);
    };

    constexpr std::uint32_t watch_mask = IN_ACCESS | IN_ATTRIB | IN_CLOSE_WRITE |
        IN_CLOSE_NOWRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY |
        IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_OPEN | IN_ONLYDIR;

    if (directories.empty()) {
        report_error("No directories were provided to DirectoryMonitor");
        cleanup_new_monitor();
        return false;
    }

    std::unordered_map<int, fs::path> new_watch_paths;
    for (const auto& directory_value : directories) {
        filesystem_error.clear();
        const fs::path directory =
            fs::absolute(directory_value, filesystem_error).lexically_normal();
        if (filesystem_error) {
            const auto message = "Cannot resolve directory '" + directory_value.string() +
                                 "': " + filesystem_error.message();
            report_error(message);
            cleanup_new_monitor();
            return false;
        }

        filesystem_error.clear();
        if (!fs::is_directory(directory, filesystem_error)) {
            std::string message = "Cannot watch directory '" + directory.string() +
                                  "': path does not exist or is not a directory";
            if (filesystem_error) {
                message += " (" + filesystem_error.message() + ")";
            }
            report_error(message);
            cleanup_new_monitor();
            return false;
        }

        const int watch_descriptor =
            inotify_add_watch(new_inotify_fd, directory.c_str(), watch_mask);
        if (watch_descriptor < 0) {
            const auto message = system_error_message(
                "Cannot watch directory '" + directory.string() + "'");
            report_error(message);
            cleanup_new_monitor();
            return false;
        }

        new_watch_paths[watch_descriptor] = directory;
    }

    if (inotify_fd_ >= 0) {
        write_log("MONITOR_RECONFIGURING");
        close(inotify_fd_);
        inotify_fd_ = -1;
    }
    watch_paths_.clear();
    if (event_log_stream_.is_open()) {
        event_log_stream_.close();
    }

    inotify_fd_ = new_inotify_fd;
    watch_paths_ = std::move(new_watch_paths);
    event_log_path_ = new_event_log_path;
    event_log_stream_ = std::move(new_event_log_stream);

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
        const auto message = system_error_message("poll on inotify descriptor failed");
        std::cerr << message << '\n';
        write_log("ERROR " + message);
        stop();
        return;
    }
    if (poll_result == 0) {
        return;
    }

    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        std::string flags;
        if ((descriptor.revents & POLLERR) != 0) flags += " POLLERR";
        if ((descriptor.revents & POLLHUP) != 0) flags += " POLLHUP";
        if ((descriptor.revents & POLLNVAL) != 0) flags += " POLLNVAL";
        const auto message = "Unexpected poll flags:" + flags;
        std::cerr << message << '\n';
        write_log("ERROR " + message);
        stop();
        return;
    }
    if ((descriptor.revents & POLLIN) == 0) {
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
            const auto message = system_error_message("read from inotify descriptor failed");
            std::cerr << message << '\n';
            write_log("ERROR " + message);
            stop();
            return;
        }
        if (bytes_read == 0) {
            break;
        }

        const auto available = static_cast<std::size_t>(bytes_read);
        std::size_t offset = 0U;
        bool malformed_record = false;

        while (offset < available) {
            if (available - offset < sizeof(inotify_event)) {
                malformed_record = true;
                break;
            }

            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
            const auto header_size = sizeof(inotify_event);
            const auto event_size = header_size + static_cast<std::size_t>(event->len);
            if (event_size > available - offset) {
                malformed_record = true;
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

            if ((event->mask & IN_Q_OVERFLOW) != 0U) {
                const std::string message =
                    "INOTIFY_QUEUE_OVERFLOW: some filesystem events may have been lost";
                std::cerr << message << '\n';
                write_log("ERROR " + message);
            } else if (target != event_log_path_) {
                write_log(describe_event(event->mask) + " " + target.string());
            }

            if ((event->mask & IN_IGNORED) != 0U) {
                watch_paths_.erase(event->wd);
            }

            offset += event_size;
        }

        if (malformed_record) {
            const std::string message = "Malformed event record received from inotify";
            std::cerr << message << '\n';
            write_log("ERROR " + message);
            stop();
            return;
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
    if (!event_log_stream_.is_open() || !event_log_stream_) {
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

    if (!event_log_stream_) {
        std::cerr << "Write error for event log '" << event_log_path_ << "'\n";
        event_log_stream_.close();
    }
}
