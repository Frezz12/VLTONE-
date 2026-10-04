#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>
namespace daw::plugins::mini {
bool runCreatorProcess(const std::filesystem::path &executable,
                       const std::vector<std::string> &arguments,
                       const std::filesystem::path &log, unsigned timeoutMs,
                       const std::atomic<bool> *cancel, std::string &error,
                       bool ownProcessGroup = true);
std::filesystem::path creatorExecutableDirectory();
std::filesystem::path creatorJobDirectory();
std::string readCreatorText(const std::filesystem::path &,
                            std::size_t maxBytes);
bool writeCreatorText(const std::filesystem::path &, const std::string &);
std::string pathUtf8(const std::filesystem::path &);
std::filesystem::path fromUtf8(std::string_view);
} // namespace daw::plugins::mini
