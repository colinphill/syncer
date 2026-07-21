#include "adb.hpp"

#include "syncer/blake3.hpp"
#include "syncer/version.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

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

std::string file_digest(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error(ExitCode::adb, "cannot read packaged Android server");
    Blake3 hash;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count != 0) hash.update(std::span(buffer.data(), count));
    }
    if (input.bad()) throw Error(ExitCode::adb, "cannot read packaged Android server");
    return digest_hex(hash.finalize());
}

std::string display_component(std::string value) {
    std::replace(value.begin(), value.end(), '_', ' ');
    return value;
}

std::string connection_kind(const AdbDevice& device) {
    if (!device.usb.empty()) return "usb";
    if (device.serial.starts_with("emulator-")) return "emulator";
    if (device.serial.find(':') != std::string::npos) return "network";
    return "unknown";
}

bool known_adb_state(std::string_view state) {
    // Keep this aligned with the connection states printed by `adb devices`.
    // Valid rows may be padded with spaces, so state validation is what keeps
    // daemon diagnostics such as "adb server version ..." out of the result.
    return state == "device" || state == "offline" || state == "unauthorized" ||
           state == "authorizing" || state == "connecting" || state == "bootloader" ||
           state == "recovery" || state == "rescue" || state == "sideload" ||
           state == "host" || state == "detached" || state == "unknown";
}

} // namespace

std::vector<AdbDevice> parse_adb_devices(std::string_view output) {
    std::vector<AdbDevice> devices;
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(std::move(line));
        if (line.empty() || line == "List of devices attached" || line.front() == '*') continue;

        const auto serial_end = line.find_first_of(" \t");
        if (serial_end == std::string::npos || serial_end == 0) continue;
        AdbDevice result;
        result.serial = line.substr(0, serial_end);
        auto details = trim(line.substr(serial_end + 1));
        if (details.empty()) continue;

        std::string metadata;
        if (details == "no permissions" || details.starts_with("no permissions ") ||
            details.starts_with("no permissions\t")) {
            result.state = "no permissions";
            metadata = trim(details.substr(result.state.size()));
        } else {
            const auto state_end = details.find_first_of(" \t");
            result.state = details.substr(0, state_end);
            metadata = state_end == std::string::npos ? std::string{} : trim(details.substr(state_end + 1));
            if (!known_adb_state(result.state)) continue;
        }

        std::istringstream fields(metadata);
        std::string field;
        while (fields >> field) {
            const auto separator = field.find(':');
            if (separator == std::string::npos || separator == 0) continue;
            const auto name = field.substr(0, separator);
            const auto value = field.substr(separator + 1);
            if (name == "model") result.model = value;
            else if (name == "product") result.product = value;
            else if (name == "device") result.device = value;
            else if (name == "transport_id") result.transport_id = value;
            else if (name == "usb") result.usb = value;
        }

        result.connection = connection_kind(result);
        result.id = (result.model.empty() ? "unknown" : result.model) + "|" + result.serial;
        const auto& friendly = !result.model.empty() ? result.model :
            (!result.product.empty() ? result.product : result.device);
        result.display_name = friendly.empty() ? result.serial :
            display_component(friendly) + " (" + result.serial + ")";
        devices.push_back(std::move(result));
    }
    return devices;
}

Adb::Adb(std::optional<fs::path> requested_path, std::optional<std::string> requested_serial)
    : executable_(locate(requested_path)) {
    serial_ = select_serial(requested_serial);
}

std::vector<AdbDevice> Adb::enumerate(const std::optional<fs::path>& requested_path) {
    const auto executable = locate(requested_path);
    const auto result = run_process(executable, {"devices", "-l"});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "adb devices failed: " + trim(result.output));
    return parse_adb_devices(result.output);
}

fs::path Adb::locate(const std::optional<fs::path>& requested) {
    if (requested) {
        if (!fs::is_regular_file(*requested)) throw Error(ExitCode::adb, "--adb does not name an existing file");
        return fs::canonical(*requested);
    }
    for (const char* variable : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (const char* value = std::getenv(variable); value && *value) {
            const fs::path candidate = path_from_utf8(value) / "platform-tools" /
#ifdef _WIN32
                "adb.exe";
#else
                "adb";
#endif
            if (fs::is_regular_file(candidate)) return candidate;
        }
    }
#ifdef _WIN32
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data) {
        const fs::path candidate = path_from_utf8(local_app_data) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (fs::is_regular_file(candidate)) return candidate;
    }
    std::wstring found(32768, L'\0');
    const auto length = SearchPathW(nullptr, L"adb.exe", nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (length > 0 && length < found.size()) { found.resize(length); return found; }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        const fs::path candidate = path_from_utf8(home) /
#if defined(__APPLE__)
            "Library" / "Android" / "sdk" / "platform-tools" / "adb";
#else
            "Android" / "Sdk" / "platform-tools" / "adb";
#endif
        if (fs::is_regular_file(candidate)) return candidate;
    }
    if (const char* path = std::getenv("PATH"); path && *path) {
        for (const auto& directory : split(path, ':')) {
            const fs::path candidate = (directory.empty() ? fs::current_path() : path_from_utf8(directory)) / "adb";
            if (fs::is_regular_file(candidate)) return fs::canonical(candidate);
        }
    }
#endif
    throw Error(ExitCode::adb, "adb was not found; use --adb or install Android platform-tools");
}

ProcessResult Adb::run(const std::vector<std::string>& arguments, bool device) const {
    std::vector<std::string> complete;
    if (device) { complete.emplace_back("-s"); complete.push_back(serial_); }
    complete.insert(complete.end(), arguments.begin(), arguments.end());
    return run_process(executable_, complete);
}

std::string Adb::select_serial(const std::optional<std::string>& requested) const {
    std::optional<std::string> desired = requested;
    if (!desired) if (const char* value = std::getenv("ANDROID_SERIAL"); value && *value) desired = value;
    if (desired) {
        const auto result = run_process(executable_, {"-s", *desired, "get-state"});
        if (result.exit_code != 0 || last_line(result.output) != "device") throw Error(ExitCode::adb, "selected ADB device is not ready: " + *desired);
        return *desired;
    }
    const auto result = run_process(executable_, {"devices", "-l"});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "adb devices failed: " + trim(result.output));
    std::vector<std::string> devices;
    for (const auto& device : parse_adb_devices(result.output))
        if (device.ready()) devices.push_back(device.serial);
    if (devices.empty()) throw Error(ExitCode::adb, "no ready ADB device was found");
    if (devices.size() != 1) throw Error(ExitCode::adb, "multiple ADB devices are ready; select one with --serial");
    return devices.front();
}

std::string Adb::query_abi() const {
    auto result = run({"shell", "getprop", "ro.product.cpu.abilist"});
    if (result.exit_code != 0 || last_line(result.output).empty()) result = run({"shell", "getprop", "ro.product.cpu.abi"});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot query Android CPU ABI");
    static const std::set<std::string> supported{"arm64-v8a", "armeabi-v7a", "x86_64", "x86"};
    for (auto abi : split(last_line(result.output), ',')) if (supported.contains(abi)) return abi;
    throw Error(ExitCode::adb, "device has no packaged server ABI: " + last_line(result.output));
}

std::string Adb::deploy_server() {
    const auto abi = query_abi();
    const auto local = executable_directory() / "servers" / abi / "syncerd";
    if (!fs::is_regular_file(local)) throw Error(ExitCode::adb, "packaged Android server is missing: " + path_to_utf8(local));
    const auto digest = file_digest(local);
    const std::string directory = std::string("/data/local/tmp/syncer/") + kVersion + "/" + digest;
    const std::string remote = directory + "/syncerd";
    auto result = run({"shell", "test", "-x", remote});
    if (result.exit_code == 0) return remote;
    result = run({"shell", "mkdir", "-p", directory});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot create Android deployment directory: " + trim(result.output));
    result = run({"push", path_to_utf8(local), remote});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot deploy Android server: " + trim(result.output));
    result = run({"shell", "chmod", "755", remote});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot make Android server executable: " + trim(result.output));
    return remote;
}

unsigned Adb::create_forward(std::string_view socket_name) {
    const auto result = run({"forward", "tcp:0", std::string("localabstract:") + std::string(socket_name)});
    if (result.exit_code != 0) throw Error(ExitCode::adb, "cannot create ADB forwarding: " + trim(result.output));
    try { return static_cast<unsigned>(std::stoul(last_line(result.output))); }
    catch (...) { throw Error(ExitCode::adb, "ADB returned an invalid forwarded port: " + last_line(result.output)); }
}

void Adb::remove_forward(unsigned port) noexcept {
    try { (void)run({"forward", "--remove", "tcp:" + std::to_string(port)}); } catch (...) {}
}

ChildProcess Adb::launch_server(std::string_view remote_path, std::string_view socket_name, std::string_view token) {
    return start_process(executable_, {"-s", serial_, "shell", std::string(remote_path), "--socket",
                                       std::string(socket_name), "--token", std::string(token)});
}

} // namespace syncer::client
