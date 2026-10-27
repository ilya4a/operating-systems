#pragma once

#include <filesystem>
#include <string>
#include <vector>

class Config {
  public:
    bool load(const std::filesystem::path &path, std::string &error_message);

    int interval_seconds() const noexcept;
    const std::vector<std::filesystem::path> &directories() const noexcept;
    const std::filesystem::path &event_log() const noexcept;
    const std::filesystem::path &pid_file() const noexcept;

  private:
    int interval_seconds_ = 5;
    std::vector<std::filesystem::path> directories_;
    std::filesystem::path event_log_;
    std::filesystem::path pid_file_;
};
