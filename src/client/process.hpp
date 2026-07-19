#pragma once

#include "syncer/error.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace syncer::client {

struct ProcessResult {
    int exit_code{};
    std::string output;
};

class ChildProcess {
public:
    ChildProcess() = default;
#ifdef _WIN32
    ChildProcess(HANDLE process, HANDLE output) : process_(process), output_handle_(output) {}
#else
    ChildProcess(pid_t process, int output) : process_(process), output_fd_(output) {}
#endif
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ~ChildProcess();
    void finish(unsigned timeout_ms = 3000) noexcept;
#ifdef _WIN32
    [[nodiscard]] bool valid() const noexcept { return process_ != nullptr; }
#else
    [[nodiscard]] bool valid() const noexcept { return process_ > 0; }
#endif
    [[nodiscard]] const std::string& output() const noexcept { return output_; }
private:
#ifdef _WIN32
    HANDLE process_{};
    HANDLE output_handle_{};
#else
    pid_t process_{};
    int output_fd_{-1};
#endif
    std::string output_;
};

class Utf8ConsoleOutput {
public:
    Utf8ConsoleOutput() noexcept;
    Utf8ConsoleOutput(const Utf8ConsoleOutput&) = delete;
    Utf8ConsoleOutput& operator=(const Utf8ConsoleOutput&) = delete;
    ~Utf8ConsoleOutput() noexcept;
private:
#ifdef _WIN32
    UINT original_code_page_{};
    bool changed_{};
#endif
};

[[nodiscard]] ProcessResult run_process(const std::filesystem::path& executable,
                                        const std::vector<std::string>& arguments);
[[nodiscard]] ChildProcess start_process(const std::filesystem::path& executable,
                                         const std::vector<std::string>& arguments);
[[nodiscard]] std::filesystem::path executable_directory();
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& value);

#ifdef _WIN32
[[nodiscard]] std::string utf8_from_wide(std::wstring_view value);
#endif

}
