#include "DirectoryMonitor.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/inotify.h>
#include <syslog.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

    std::string describe_event(std::uint32_t mask) {
        std::vector<std::string> names;

        if ((mask & IN_ACCESS) != 0U) {
            names.emplace_back("ACCESS");
        }
        if ((mask & IN_ATTRIB) != 0U) {
            names.emplace_back("ATTRIB");
        }
        if ((mask & IN_CLOSE_WRITE) != 0U) {
            names.emplace_back("CLOSE_WRITE");
        }
        if ((mask & IN_CLOSE_NOWRITE) != 0U) {
            names.emplace_back("CLOSE_NOWRITE");
        }
        if ((mask & IN_CREATE) != 0U) {
            names.emplace_back("CREATE");
        }
        if ((mask & IN_DELETE) != 0U) {
            names.emplace_back("DELETE");
        }
        if ((mask & IN_DELETE_SELF) != 0U) {
            names.emplace_back("DELETE_SELF");
        }
        if ((mask & IN_MODIFY) != 0U) {
            names.emplace_back("MODIFY");
        }
        if ((mask & IN_MOVE_SELF) != 0U) {
            names.emplace_back("MOVE_SELF");
        }
        if ((mask & IN_MOVED_FROM) != 0U) {
            names.emplace_back("MOVED_FROM");
        }
        if ((mask & IN_MOVED_TO) != 0U) {
            names.emplace_back("MOVED_TO");
        }
        if ((mask & IN_OPEN) != 0U) {
            names.emplace_back("OPEN");
        }
        if ((mask & IN_IGNORED) != 0U) {
            names.emplace_back("IGNORED");
        }
        if ((mask & IN_UNMOUNT) != 0U) {
            names.emplace_back("UNMOUNT");
        }
        if ((mask & IN_Q_OVERFLOW) != 0U) {
            names.emplace_back("QUEUE_OVERFLOW");
        }

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

    std::string system_error_message(const std::string &operation) {
        const int error_number = errno;
        return operation + ": " + std::strerror(error_number);
    }

} // namespace

DirectoryMonitor::~DirectoryMonitor() {
    stop();
}

bool DirectoryMonitor::start(
    const std::vector<std::filesystem::path> &directories,
    const std::filesystem::path &event_log
) {
    namespace fs = std::filesystem;

    if (directories.empty()) {
        report_error("No directories were provided to DirectoryMonitor");
        return false;
    }
    if (event_log.empty() || !event_log.is_absolute()) {
        report_error("Event-log path must be a non-empty absolute path: '" + event_log.string() + "'");
        return false;
    }
    for (const auto &directory : directories) {
        if (directory.empty() || !directory.is_absolute()) {
            report_error("Directory path must be a non-empty absolute path: '" + directory.string() + "'");
            return false;
        }
    }

    const fs::path new_event_log_path = event_log.lexically_normal();
    std::ofstream new_event_log_stream;
    new_event_log_stream.open(new_event_log_path, std::ios::out | std::ios::app);
    if (!new_event_log_stream.is_open()) {
        report_error("Cannot open event log '" + new_event_log_path.string() + "'");
        return false;
    }

    const int new_inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (new_inotify_fd < 0) {
        report_error(system_error_message("inotify_init1 failed"));
        return false;
    }

    const auto cleanup_new_monitor = [&new_inotify_fd]() { close(new_inotify_fd); };

    constexpr std::uint32_t watch_mask = IN_ACCESS | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_CREATE
                                         | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM
                                         | IN_MOVED_TO | IN_OPEN | IN_ONLYDIR;

    std::unordered_map<int, fs::path> new_watch_paths;
    for (const auto &directory_value : directories) {
        const fs::path directory = directory_value.lexically_normal();
        std::error_code filesystem_error;
        if (!fs::is_directory(directory, filesystem_error)) {
            std::string message =
                "Cannot watch directory '" + directory.string() + "': path does not exist or is not a directory";
            if (filesystem_error) {
                message += " (" + filesystem_error.message() + ")";
            }
            report_error(message);
            cleanup_new_monitor();
            return false;
        }

        const int watch_descriptor = inotify_add_watch(new_inotify_fd, directory.c_str(), watch_mask);
        if (watch_descriptor < 0) {
            const auto message = system_error_message("Cannot watch directory '" + directory.string() + "'");
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
    syslog(
        LOG_INFO,
        "Directory monitor started; watching %zu director%s",
        watch_paths_.size(),
        watch_paths_.size() == 1U ? "y" : "ies"
    );
    return true;
}

bool DirectoryMonitor::process_events(int interrupt_fd) {
    if (inotify_fd_ < 0) {
        return false;
    }
    if (interrupt_fd < 0) {
        report_error("Invalid interrupt descriptor passed to DirectoryMonitor");
        stop();
        return false;
    }

    pollfd descriptors[2] {};
    descriptors[0].fd = inotify_fd_;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = interrupt_fd;
    descriptors[1].events = POLLIN;

    const int poll_result = poll(descriptors, 2, -1);
    if (poll_result < 0) {
        if (errno == EINTR) {
            return true;
        }
        report_error(system_error_message("poll on inotify/signal descriptors failed"));
        stop();
        return false;
    }

    if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        report_error("Unexpected poll flags on signal descriptor");
        stop();
        return false;
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        std::string flags;
        if ((descriptors[0].revents & POLLERR) != 0) {
            flags += " POLLERR";
        }
        if ((descriptors[0].revents & POLLHUP) != 0) {
            flags += " POLLHUP";
        }
        if ((descriptors[0].revents & POLLNVAL) != 0) {
            flags += " POLLNVAL";
        }
        report_error("Unexpected poll flags on inotify descriptor:" + flags);
        stop();
        return false;
    }
    if ((descriptors[0].revents & POLLIN) == 0) {
        return true;
    }

    alignas(inotify_event) std::array<char, 64U * 1024U> buffer {};
    while (true) {
        const auto bytes_read = read(inotify_fd_, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            report_error(system_error_message("read from inotify descriptor failed"));
            stop();
            return false;
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

            const auto *event = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
            const auto event_size = sizeof(inotify_event) + static_cast<std::size_t>(event->len);
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
                report_error("INOTIFY_QUEUE_OVERFLOW: some filesystem events may have been lost");
            } else if (target != event_log_path_) {
                write_log(describe_event(event->mask) + " " + target.string());
            }

            if ((event->mask & IN_IGNORED) != 0U) {
                watch_paths_.erase(event->wd);
            }

            offset += event_size;
        }

        if (malformed_record) {
            report_error("Malformed event record received from inotify");
            stop();
            return false;
        }
    }

    return true;
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

void DirectoryMonitor::report_error(const std::string &message) {
    syslog(LOG_ERR, "%s", message.c_str());
    std::cerr << message << '\n';
    if (event_log_stream_.is_open()) {
        write_log("ERROR " + message);
    }
}

void DirectoryMonitor::write_log(const std::string &message) {
    if (!event_log_stream_.is_open() || !event_log_stream_) {
        return;
    }

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local_time {};
    if (localtime_r(&now, &local_time) == nullptr) {
        event_log_stream_ << "[time-unavailable] ";
    } else {
        event_log_stream_ << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << ' ';
    }
    event_log_stream_ << message << '\n';
    event_log_stream_.flush();

    if (!event_log_stream_) {
        const std::string error_message = "Write error for event log '" + event_log_path_.string() + "'";
        syslog(LOG_ERR, "%s", error_message.c_str());
        std::cerr << error_message << '\n';
        event_log_stream_.close();
    }
}

