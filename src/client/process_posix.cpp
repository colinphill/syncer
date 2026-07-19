#include "process.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace syncer::client {
namespace fs = std::filesystem;
namespace {

struct SpawnedProcess {
    pid_t process{};
    int output_fd{-1};
};

std::string posix_error(std::string_view operation, int error = errno) {
    return std::string(operation) + ": " + std::strerror(error);
}

SpawnedProcess spawn_process(const fs::path& executable, const std::vector<std::string>& arguments,
                             bool null_stdin) {
    int output_pipe[2]{};
    if (pipe(output_pipe) != 0) throw Error(ExitCode::adb, posix_error("cannot create process pipe"));
    for (auto& descriptor : output_pipe) {
        if (descriptor > STDERR_FILENO) continue;
        const int replacement = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) {
            const int error = errno;
            close(output_pipe[0]);
            close(output_pipe[1]);
            throw Error(ExitCode::adb, posix_error("cannot configure process pipe", error));
        }
        close(descriptor);
        descriptor = replacement;
    }

    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        throw Error(ExitCode::adb, posix_error("cannot prepare process", result));
    }
    result = posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    if (result == 0) result = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    if (result == 0) result = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
    if (result == 0) result = posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    if (result == 0 && null_stdin)
        result = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(output_pipe[0]);
        close(output_pipe[1]);
        throw Error(ExitCode::adb, posix_error("cannot prepare process", result));
    }

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable.native());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);

    pid_t process{};
    result = posix_spawn(&process, storage.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output_pipe[1]);
    if (result != 0) {
        close(output_pipe[0]);
        throw Error(ExitCode::adb, posix_error("cannot start process", result));
    }
    return {process, output_pipe[0]};
}

int process_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace

Utf8ConsoleOutput::Utf8ConsoleOutput() noexcept = default;
Utf8ConsoleOutput::~Utf8ConsoleOutput() noexcept = default;

fs::path path_from_utf8(std::string_view value) { return fs::path(std::string(value)); }

std::string path_to_utf8(const fs::path& value) {
    const auto encoded = value.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

fs::path executable_directory() {
#if defined(__APPLE__)
    std::uint32_t size{};
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        throw Error(ExitCode::adb, "cannot determine client installation directory");
    std::error_code error;
    const auto executable = fs::canonical(fs::path(buffer.data()), error);
    if (error) throw Error(ExitCode::adb, "cannot determine client installation directory");
    return executable.parent_path();
#elif defined(__linux__)
    std::vector<char> buffer(1024);
    while (true) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) throw Error(ExitCode::adb, posix_error("cannot determine client installation directory"));
        if (static_cast<std::size_t>(length) < buffer.size())
            return fs::path(std::string(buffer.data(), static_cast<std::size_t>(length))).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#else
#error Unsupported POSIX client platform
#endif
}

ProcessResult run_process(const fs::path& executable, const std::vector<std::string>& arguments) {
    const auto child = spawn_process(executable, arguments, false);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto count = read(child.output_fd, buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count == 0) break;
        else if (errno != EINTR) {
            const int error = errno;
            close(child.output_fd);
            int status{};
            while (waitpid(child.process, &status, 0) < 0 && errno == EINTR) {}
            throw Error(ExitCode::adb, posix_error("cannot read process output", error));
        }
    }
    close(child.output_fd);
    int status{};
    while (waitpid(child.process, &status, 0) < 0) {
        if (errno != EINTR) throw Error(ExitCode::adb, posix_error("cannot wait for process"));
    }
    return ProcessResult{process_exit_code(status), std::move(output)};
}

ChildProcess start_process(const fs::path& executable, const std::vector<std::string>& arguments) {
    const auto child = spawn_process(executable, arguments, true);
    const int flags = fcntl(child.output_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(child.output_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        const int error = errno;
        kill(child.process, SIGKILL);
        int status{};
        while (waitpid(child.process, &status, 0) < 0 && errno == EINTR) {}
        close(child.output_fd);
        throw Error(ExitCode::adb, posix_error("cannot configure process output", error));
    }
    return ChildProcess(child.process, child.output_fd);
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(std::exchange(other.process_, 0)),
      output_fd_(std::exchange(other.output_fd_, -1)),
      output_(std::move(other.output_)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        finish();
        process_ = std::exchange(other.process_, 0);
        output_fd_ = std::exchange(other.output_fd_, -1);
        output_ = std::move(other.output_);
    }
    return *this;
}

ChildProcess::~ChildProcess() { finish(); }

void ChildProcess::finish(unsigned timeout_ms) noexcept {
    const auto drain_output = [&] {
        if (output_fd_ < 0) return;
        std::array<char, 4096> buffer{};
        while (true) {
            const auto count = read(output_fd_, buffer.data(), buffer.size());
            if (count > 0) output_.append(buffer.data(), static_cast<std::size_t>(count));
            else if (count < 0 && errno == EINTR) continue;
            else break;
        }
    };
    try {
        if (process_ > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            int status{};
            while (true) {
                drain_output();
                const auto waited = waitpid(process_, &status, WNOHANG);
                if (waited == process_ || (waited < 0 && errno == ECHILD)) break;
                if (waited < 0 && errno != EINTR) break;
                if (std::chrono::steady_clock::now() >= deadline) {
                    kill(process_, SIGKILL);
                    while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {}
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            process_ = 0;
        }
        drain_output();
    } catch (...) {}
    if (output_fd_ >= 0) {
        close(output_fd_);
        output_fd_ = -1;
    }
}

} // namespace syncer::client
