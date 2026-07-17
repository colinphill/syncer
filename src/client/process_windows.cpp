#include "process_windows.hpp"

#include <array>
#include <cwctype>
#include <utility>

namespace syncer::client {
namespace fs = std::filesystem;
namespace {

std::wstring command_line(const fs::path& executable, const std::vector<std::wstring>& arguments) {
    std::wstring result = quote_argument(executable.wstring());
    for (const auto& argument : arguments) { result.push_back(L' '); result += quote_argument(argument); }
    return result;
}

std::string windows_error(std::string_view operation) {
    return std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

} // namespace

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw Error(ExitCode::usage, "invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw Error(ExitCode::usage, "invalid Windows Unicode text");
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring quote_argument(std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == value.npos) return std::wstring(value);
    std::wstring output(1, L'\"');
    std::size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++backslashes; continue; }
        if (character == L'\"') output.append(backslashes * 2 + 1, L'\\');
        else output.append(backslashes, L'\\');
        backslashes = 0;
        output.push_back(character);
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

ProcessResult run_process(const fs::path& executable, const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe{}, write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) throw Error(ExitCode::adb, windows_error("cannot create process pipe"));
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    auto command = command_line(executable, arguments);
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(write_pipe);
    if (!created) { CloseHandle(read_pipe); throw Error(ExitCode::adb, windows_error("cannot start process")); }
    CloseHandle(info.hThread);
    std::string output;
    std::array<char, 4096> buffer{};
    DWORD read{};
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0)
        output.append(buffer.data(), read);
    CloseHandle(read_pipe);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exit_code{};
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    return ProcessResult{static_cast<int>(exit_code), std::move(output)};
}

ChildProcess start_process(const fs::path& executable, const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe{}, write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
        throw Error(ExitCode::adb, windows_error("cannot create Android server output pipe"));
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        throw Error(ExitCode::adb, windows_error("cannot open null device for Android server"));
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_handle; startup.hStdOutput = write_pipe; startup.hStdError = write_pipe;
    PROCESS_INFORMATION info{};
    auto command = command_line(executable, arguments);
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(null_handle);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        throw Error(ExitCode::adb, windows_error("cannot launch Android server"));
    }
    CloseHandle(info.hThread);
    return ChildProcess(info.hProcess, read_pipe);
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)),
      output_handle_(std::exchange(other.output_handle_, nullptr)),
      output_(std::move(other.output_)) {}
ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        finish();
        process_ = std::exchange(other.process_, nullptr);
        output_handle_ = std::exchange(other.output_handle_, nullptr);
        output_ = std::move(other.output_);
    }
    return *this;
}
ChildProcess::~ChildProcess() { finish(); }
void ChildProcess::finish(unsigned timeout_ms) noexcept {
    if (process_) {
        if (WaitForSingleObject(process_, timeout_ms) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, timeout_ms);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (output_handle_) {
        try {
            std::array<char, 4096> buffer{};
            DWORD read{};
            while (ReadFile(output_handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0)
                output_.append(buffer.data(), read);
        } catch (...) {}
        CloseHandle(output_handle_);
        output_handle_ = nullptr;
    }
}

} // namespace syncer::client
