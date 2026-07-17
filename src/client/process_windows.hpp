#pragma once

#include "syncer/error.hpp"

#include <windows.h>

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
    ChildProcess(HANDLE process, HANDLE output) : process_(process), output_handle_(output) {}
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ~ChildProcess();
    void finish(unsigned timeout_ms = 3000) noexcept;
    [[nodiscard]] bool valid() const noexcept { return process_ != nullptr; }
    [[nodiscard]] const std::string& output() const noexcept { return output_; }
private:
    HANDLE process_{};
    HANDLE output_handle_{};
    std::string output_;
};

[[nodiscard]] ProcessResult run_process(const std::filesystem::path& executable,
                                        const std::vector<std::wstring>& arguments);
[[nodiscard]] ChildProcess start_process(const std::filesystem::path& executable,
                                         const std::vector<std::wstring>& arguments);
[[nodiscard]] std::wstring widen(std::string_view value);
[[nodiscard]] std::string narrow(std::wstring_view value);
[[nodiscard]] std::wstring quote_argument(std::wstring_view value);

}
