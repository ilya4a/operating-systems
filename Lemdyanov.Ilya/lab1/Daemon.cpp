#include "Daemon.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <utility>

namespace {

    std::string error_message(const std::string &operation, int error_number) {
        return operation + ": " + std::strerror(error_number);
    }

    void report_error(const std::string &message) {
        std::cerr << message << '\n';
        syslog(LOG_ERR, "%s", message.c_str());
    }

    bool same_file(const struct stat &left, const struct stat &right) {
        return left.st_dev == right.st_dev && left.st_ino == right.st_ino && S_ISREG(left.st_mode)
               && S_ISREG(right.st_mode);
    }

    bool write_all(int descriptor, const void *data, std::size_t size) {
        const auto *bytes = static_cast<const char *>(data);
        std::size_t offset = 0U;
        while (offset < size) {
            const ssize_t written = write(descriptor, bytes + offset, size - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                if (written == 0) {
                    errno = EIO;
                }
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool read_all(int descriptor, void *data, std::size_t size) {
        auto *bytes = static_cast<char *>(data);
        std::size_t offset = 0U;
        while (offset < size) {
            const ssize_t count = read(descriptor, bytes + offset, size - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                if (count == 0) {
                    errno = EPIPE;
                }
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool write_pid_value(
        int descriptor,
        const std::filesystem::path &path,
        dev_t expected_device,
        ino_t expected_inode,
        pid_t pid,
        std::string &error
    ) {
        struct stat descriptor_status {};
        if (fstat(descriptor, &descriptor_status) < 0) {
            const int error_number = errno;
            error = error_message("Cannot inspect PID file '" + path.string() + "'", error_number);
            return false;
        }

        struct stat path_status {};
        if (lstat(path.c_str(), &path_status) < 0) {
            const int error_number = errno;
            error = error_message("Cannot inspect PID-file path '" + path.string() + "'", error_number);
            return false;
        }

        if (!S_ISREG(descriptor_status.st_mode) || !S_ISREG(path_status.st_mode)
            || descriptor_status.st_dev != expected_device || descriptor_status.st_ino != expected_inode
            || path_status.st_dev != expected_device || path_status.st_ino != expected_inode) {
            error = "PID-file path '" + path.string() + "' no longer refers to the file opened by this process";
            return false;
        }

        const std::string contents = std::to_string(static_cast<long long>(pid)) + "\n";
        if (ftruncate(descriptor, 0) < 0) {
            const int error_number = errno;
            error = error_message("Cannot truncate PID file '" + path.string() + "'", error_number);
            return false;
        }

        std::size_t offset = 0U;
        while (offset < contents.size()) {
            const ssize_t written =
                pwrite(descriptor, contents.data() + offset, contents.size() - offset, static_cast<off_t>(offset));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                const int error_number = written == 0 ? EIO : errno;
                error = error_message("Cannot write PID file '" + path.string() + "'", error_number);
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }

        if (fsync(descriptor) < 0) {
            const int error_number = errno;
            error = error_message("Cannot synchronize PID file '" + path.string() + "'", error_number);
            return false;
        }
        return true;
    }

    bool duplicate_to(int source, int destination) {
        while (dup2(source, destination) < 0) {
            if (errno != EINTR) {
                return false;
            }
        }
        return true;
    }

    bool redirect_standard_streams() {
        const int null_descriptor = open("/dev/null", O_RDWR);
        if (null_descriptor < 0) {
            return false;
        }

        for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
            if (!duplicate_to(null_descriptor, descriptor)) {
                const int error_number = errno;
                if (null_descriptor > STDERR_FILENO) {
                    close(null_descriptor);
                }
                errno = error_number;
                return false;
            }
        }

        if (null_descriptor > STDERR_FILENO) {
            close(null_descriptor);
        }
        return true;
    }

    void close_unneeded_descriptors(const std::vector<int> &descriptors_to_keep) {
        std::vector<int> keep;
        keep.reserve(descriptors_to_keep.size());
        for (const int descriptor : descriptors_to_keep) {
            if (descriptor >= STDERR_FILENO + 1) {
                keep.push_back(descriptor);
            }
        }
        std::sort(keep.begin(), keep.end());
        keep.erase(std::unique(keep.begin(), keep.end()), keep.end());

#if defined(SYS_close_range)
        bool close_range_succeeded = true;
        unsigned int first = static_cast<unsigned int>(STDERR_FILENO + 1);
        for (const int descriptor : keep) {
            const auto keep_descriptor = static_cast<unsigned int>(descriptor);
            if (first < keep_descriptor && syscall(SYS_close_range, first, keep_descriptor - 1U, 0U) < 0) {
                close_range_succeeded = false;
                break;
            }
            first = keep_descriptor + 1U;
        }
        if (close_range_succeeded && syscall(SYS_close_range, first, UINT_MAX, 0U) == 0) {
            return;
        }
#endif

        long maximum_descriptor = sysconf(_SC_OPEN_MAX);
        if (maximum_descriptor < 0) {
            maximum_descriptor = 65536;
        }
        if (maximum_descriptor > static_cast<long>(INT_MAX)) {
            maximum_descriptor = static_cast<long>(INT_MAX);
        }
        for (long descriptor = STDERR_FILENO + 1L; descriptor < maximum_descriptor; ++descriptor) {
            if (!std::binary_search(keep.begin(), keep.end(), static_cast<int>(descriptor))) {
                close(static_cast<int>(descriptor));
            }
        }
    }

    [[noreturn]] void notify_parent_and_exit(int notification_descriptor, int error_number) noexcept {
        const int status = (error_number == 0) ? 0 : error_number;
        (void) write_all(notification_descriptor, &status, sizeof(status));
        close(notification_descriptor);
        _exit(status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

} // namespace

Daemon &Daemon::instance() noexcept {
    static Daemon daemon;
    return daemon;
}

Daemon::~Daemon() {
    release_pid_file_impl(false);
}

bool Daemon::prepare_pid_file(const std::string &path_string) {
    if (pid_file_fd_ >= 0 || lock_file_fd_ >= 0) {
        report_error("PID file has already been prepared by this Daemon instance");
        return false;
    }
    if (path_string.empty()) {
        report_error("PID-file path is empty");
        return false;
    }

    const std::filesystem::path path(path_string);
    if (!path.is_absolute()) {
        report_error("PID-file path must be absolute: '" + path.string() + "'");
        return false;
    }

    const std::filesystem::path lock_path(path.string() + ".lock");
    const int lock_descriptor = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_descriptor < 0) {
        const int error_number = errno;
        report_error(error_message("Cannot open PID lock file '" + lock_path.string() + "'", error_number));
        return false;
    }

    struct stat lock_descriptor_status {};
    if (fstat(lock_descriptor, &lock_descriptor_status) < 0) {
        const int error_number = errno;
        close(lock_descriptor);
        report_error(error_message("Cannot inspect PID lock file '" + lock_path.string() + "'", error_number));
        return false;
    }
    if (!S_ISREG(lock_descriptor_status.st_mode)) {
        close(lock_descriptor);
        report_error("PID lock-file path '" + lock_path.string() + "' is not a regular file");
        return false;
    }

    int lock_result;
    do {
        lock_result = flock(lock_descriptor, LOCK_EX | LOCK_NB);
    } while (lock_result < 0 && errno == EINTR);
    if (lock_result < 0) {
        const int error_number = errno;
        close(lock_descriptor);
        if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
            report_error("PID file '" + path.string() + "' is locked by another running instance");
        } else {
            report_error(error_message("Cannot lock PID lock file '" + lock_path.string() + "'", error_number));
        }
        return false;
    }

    struct stat lock_path_status {};
    if (lstat(lock_path.c_str(), &lock_path_status) < 0) {
        const int error_number = errno;
        close(lock_descriptor);
        report_error(error_message("Cannot inspect PID lock-file path '" + lock_path.string() + "'", error_number));
        return false;
    }
    if (!same_file(lock_descriptor_status, lock_path_status)) {
        close(lock_descriptor);
        report_error("PID lock-file path changed while acquiring its lock");
        return false;
    }

    const int pid_descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (pid_descriptor < 0) {
        const int error_number = errno;
        close(lock_descriptor);
        report_error(error_message("Cannot open PID file '" + path.string() + "'", error_number));
        return false;
    }

    struct stat pid_descriptor_status {};
    if (fstat(pid_descriptor, &pid_descriptor_status) < 0) {
        const int error_number = errno;
        close(pid_descriptor);
        close(lock_descriptor);
        report_error(error_message("Cannot inspect PID file '" + path.string() + "'", error_number));
        return false;
    }
    if (!S_ISREG(pid_descriptor_status.st_mode)) {
        close(pid_descriptor);
        close(lock_descriptor);
        report_error("PID-file path '" + path.string() + "' is not a regular file");
        return false;
    }

    struct stat pid_path_status {};
    if (lstat(path.c_str(), &pid_path_status) < 0) {
        const int error_number = errno;
        close(pid_descriptor);
        close(lock_descriptor);
        report_error(error_message("Cannot inspect PID-file path '" + path.string() + "'", error_number));
        return false;
    }
    if (!same_file(pid_descriptor_status, pid_path_status)) {
        close(pid_descriptor);
        close(lock_descriptor);
        report_error("PID-file path changed while acquiring the file");
        return false;
    }

    std::string write_error;
    if (!write_pid_value(
            pid_descriptor,
            path,
            pid_descriptor_status.st_dev,
            pid_descriptor_status.st_ino,
            getpid(),
            write_error
        )) {
        close(pid_descriptor);
        close(lock_descriptor);
        report_error(write_error);
        return false;
    }

    pid_file_fd_ = pid_descriptor;
    lock_file_fd_ = lock_descriptor;
    pid_file_path_ = path;
    pid_file_device_ = pid_descriptor_status.st_dev;
    pid_file_inode_ = pid_descriptor_status.st_ino;
    syslog(LOG_INFO, "Acquired PID-file lock at '%s' for PID %ld", pid_file_path_.c_str(), static_cast<long>(getpid()));
    return true;
}

bool Daemon::update_pid_after_fork() {
    if (pid_file_fd_ < 0) {
        return true;
    }
    std::string error;
    if (!write_pid_value(pid_file_fd_, pid_file_path_, pid_file_device_, pid_file_inode_, getpid(), error)) {
        report_error(error);
        return false;
    }
    syslog(LOG_INFO, "Updated PID file '%s' to daemon PID %ld", pid_file_path_.c_str(), static_cast<long>(getpid()));
    return true;
}

bool Daemon::daemonize() {
    return daemonize({});
}

bool Daemon::daemonize(const std::vector<int> &additional_descriptors_to_keep) {
    int startup_pipe[2] = { -1, -1 };
    if (pipe2(startup_pipe, O_CLOEXEC) < 0) {
        const int error_number = errno;
        report_error(error_message("Cannot create daemon startup pipe", error_number));
        return false;
    }

    for (int index = 0; index < 2; ++index) {
        if (startup_pipe[index] <= STDERR_FILENO) {
            const int replacement = fcntl(startup_pipe[index], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (replacement < 0) {
                const int error_number = errno;
                close(startup_pipe[0]);
                close(startup_pipe[1]);
                report_error(error_message("Cannot preserve daemon startup pipe", error_number));
                return false;
            }
            close(startup_pipe[index]);
            startup_pipe[index] = replacement;
        }
    }

    const pid_t child_pid = fork();
    if (child_pid < 0) {
        const int error_number = errno;
        close(startup_pipe[0]);
        close(startup_pipe[1]);
        report_error(error_message("fork failed", error_number));
        return false;
    }

    if (child_pid > 0) {
        close(startup_pipe[1]);
        int child_status = EIO;
        const bool received_status = read_all(startup_pipe[0], &child_status, sizeof(child_status));
        close(startup_pipe[0]);
        if (!received_status) {
            child_status = EIO;
        }
        _exit(child_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    const int notification_descriptor = startup_pipe[1];
    close(startup_pipe[0]);

    const auto fail_in_child = [this, notification_descriptor](int error_number, const char *operation) {
        const std::string message = error_message(operation, error_number);
        report_error(message);
        release_pid_file_impl(true);
        notify_parent_and_exit(notification_descriptor, error_number);
    };

    if (setsid() < 0) {
        fail_in_child(errno, "setsid failed");
    }
    if (chdir("/") < 0) {
        fail_in_child(errno, "chdir to / failed");
    }
    (void) umask(027);

    if (!update_pid_after_fork()) {
        release_pid_file_impl(true);
        notify_parent_and_exit(notification_descriptor, EIO);
    }

    closelog();
    std::vector<int> descriptors_to_keep = additional_descriptors_to_keep;
    descriptors_to_keep.push_back(notification_descriptor);
    if (pid_file_fd_ >= 0) {
        descriptors_to_keep.push_back(pid_file_fd_);
    }
    if (lock_file_fd_ >= 0) {
        descriptors_to_keep.push_back(lock_file_fd_);
    }
    close_unneeded_descriptors(descriptors_to_keep);

    openlog("disk-monitor", LOG_PID | LOG_NDELAY, LOG_LOCAL0);
    if (!redirect_standard_streams()) {
        const int error_number = errno;
        report_error(error_message("Cannot redirect standard streams to /dev/null", error_number));
        release_pid_file_impl(true);
        notify_parent_and_exit(notification_descriptor, error_number);
    }

    const int success_status = 0;
    if (!write_all(notification_descriptor, &success_status, sizeof(success_status))) {
        release_pid_file_impl(true);
        close(notification_descriptor);
        _exit(EXIT_FAILURE);
    }
    close(notification_descriptor);

    syslog(LOG_INFO, "Daemonization completed; daemon PID %ld", static_cast<long>(getpid()));
    return true;
}

void Daemon::release_pid_file() noexcept {
    release_pid_file_impl(true);
}

void Daemon::release_pid_file_impl(bool report_errors) noexcept {
    if (pid_file_fd_ >= 0) {
        struct stat path_status {};
        if (lstat(pid_file_path_.c_str(), &path_status) == 0) {
            if (S_ISREG(path_status.st_mode) && path_status.st_dev == pid_file_device_
                && path_status.st_ino == pid_file_inode_) {
                if (unlink(pid_file_path_.c_str()) < 0 && errno != ENOENT && report_errors) {
                    const int error_number = errno;
                    syslog(
                        LOG_WARNING,
                        "Cannot remove PID file '%s': %s",
                        pid_file_path_.c_str(),
                        std::strerror(error_number)
                    );
                }
            } else if (report_errors) {
                syslog(
                    LOG_WARNING,
                    "PID-file path '%s' no longer refers to the file created by this daemon; leaving it untouched",
                    pid_file_path_.c_str()
                );
            }
        } else if (errno != ENOENT && report_errors) {
            const int error_number = errno;
            syslog(
                LOG_WARNING,
                "Cannot inspect PID file '%s' during cleanup: %s",
                pid_file_path_.c_str(),
                std::strerror(error_number)
            );
        }

        if (close(pid_file_fd_) < 0 && report_errors) {
            const int error_number = errno;
            syslog(LOG_WARNING, "Cannot close PID file '%s': %s", pid_file_path_.c_str(), std::strerror(error_number));
        }
        pid_file_fd_ = -1;
    }

    if (lock_file_fd_ >= 0) {
        if (close(lock_file_fd_) < 0 && report_errors) {
            const int error_number = errno;
            syslog(
                LOG_WARNING,
                "Cannot close PID lock file for '%s': %s",
                pid_file_path_.c_str(),
                std::strerror(error_number)
            );
        }
        lock_file_fd_ = -1;
    }
}
