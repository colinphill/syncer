#include "adb.hpp"

#include "syncer/version.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace syncer::client {
namespace fs = std::filesystem;
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::size_t start{};
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    value.erase(0, start);
    return value;
}

std::string last_line(const std::string& value) {
    std::istringstream lines(value);
    std::string line, last;
    while (std::getline(lines, line)) if (!trim(line).empty()) last = trim(line);
    return last;
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> result;
    std::size_t start{};
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        result.emplace_back(value.substr(start, end == value.npos ? value.size() - start : end - start));
        if (end == value.npos) break;
        start = end + 1;
    }
    return result;
}

fs::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) throw Error(ExitCode::adb, "cannot determine client installation directory");
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

} // namespace

Adb::Adb(std::optional<fs::path> requested_path, std::optional<std::string> requested_serial)
    : executable_(locate(requested_path)) {
    serial_ = select_serial(requested_serial);
}

fs::path Adb::locate(const std::optional<fs::path>& requested) {
    if (requested) {
        if (!fs::is_regular_file(*requested)) throw Error(ExitCode::adb, "--adb does not name an existing file");
        return fs::canonical(*requested);
    }
    for (const char* variable : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (const char* value = std::getenv(variable); value && *value) {
            const fs::path candidate = fs::path(value) / "platform-tools" / "adb.exe";
            if (fs::is_regular_file(candidate)) return candidate;
        }
    }
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data) {
        const fs::path candidate = fs::path(local_app_data) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (fs::is_regular_file(candidate)) return candidate;
    }
    std::wstring found(32768, L'\0');
    const auto length = SearchPathW(nullptr, L"adb.exe", nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (length > 0 && length < found.size()) { found.resize(length); return found; }
    throw Error(ExitCode::adb, "adb.exe was not found; use --adb or install Android platform-tools");
}

ProcessResult Adb::run(const std::vector<std::wstring>& arguments, bool device) const {
    std::vector<std::wstring> complete;
    if (device) { complete.emplace_back(L"-s"); complete.push_back(widen(serial_)); }
    complete.insert(complete.end(), arguments.begin(), arguments.end());
    return run_process(executable_, complete);
}

std::string Adb::select_serial(const std::optional<std::string>& requested) const {
    std::optional<std::string> desired = requested;
    if (!desired) if (const char* value = std::getenv("ANDROID_SERIAL"); value && *value) desired = value;
    if (desired) {
        const auto result = run_process(executable_, {L"-s", widen(*desired), L"get-state"});
        if (result.exit_code != 0 || last_line(result.output) != "device") throw Error(ExitCode::adb, "selected ADB device is not ready: " + *desired);
        return *desired;
    }
    const auto result = run_process(executable_, {L"devices"});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "adb devices failed: " + trim(result.output));
    std::vector<std::string> devices;
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto tab = line.find('\t');
        if (tab != line.npos && trim(line.substr(tab + 1)) == "device") devices.push_back(line.substr(0, tab));
    }
    if (devices.empty()) throw Error(ExitCode::adb, "no ready ADB device was found");
    if (devices.size() != 1) throw Error(ExitCode::adb, "multiple ADB devices are ready; select one with --serial");
    return devices.front();
}

std::string Adb::query_abi() const {
    auto result = run({L"shell", L"getprop", L"ro.product.cpu.abilist"});
    if (result.exit_code != 0 || last_line(result.output).empty()) result = run({L"shell", L"getprop", L"ro.product.cpu.abi"});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot query Android CPU ABI");
    static const std::set<std::string> supported{"arm64-v8a", "armeabi-v7a", "x86_64", "x86"};
    for (auto abi : split(last_line(result.output), ',')) if (supported.contains(abi)) return abi;
    throw Error(ExitCode::adb, "device has no packaged server ABI: " + last_line(result.output));
}

std::string Adb::deploy_server() {
    const auto abi = query_abi();
    const auto local = executable_directory() / "servers" / widen(abi) / "syncerd";
    if (!fs::is_regular_file(local)) throw Error(ExitCode::adb, "packaged Android server is missing: " + narrow(local.wstring()));
    const std::string directory = std::string("/data/local/tmp/syncer/") + kVersion;
    const std::string remote = directory + "/syncerd";
    auto result = run({L"shell", L"mkdir", L"-p", widen(directory)});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot create Android deployment directory: " + trim(result.output));
    result = run({L"push", local.wstring(), widen(remote)});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot deploy Android server: " + trim(result.output));
    result = run({L"shell", L"chmod", L"755", widen(remote)});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot make Android server executable: " + trim(result.output));
    return remote;
}

unsigned Adb::create_forward(std::string_view socket_name) {
    const auto result = run({L"forward", L"tcp:0", widen(std::string("localabstract:") + std::string(socket_name))});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot create ADB forwarding: " + trim(result.output));
    try { return static_cast<unsigned>(std::stoul(last_line(result.output))); }
    catch (...) { throw Error(ExitCode::adb, "ADB returned an invalid forwarded port: " + last_line(result.output)); }
}

void Adb::remove_forward(unsigned port) noexcept {
    try { (void)run({L"forward", L"--remove", L"tcp:" + std::to_wstring(port)}); } catch (...) {}
}

ChildProcess Adb::launch_server(std::string_view remote_path, std::string_view socket_name, std::string_view token) {
    return start_process(executable_, {L"-s", widen(serial_), L"shell", widen(remote_path), L"--socket",
                                       widen(socket_name), L"--token", widen(token)});
}

} // namespace syncer::client
