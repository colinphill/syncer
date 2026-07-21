#pragma once

#include "process.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncer::client {

struct AdbDevice {
    std::string id;
    std::string serial;
    std::string display_name;
    std::string state;
    std::string model;
    std::string product;
    std::string device;
    std::string transport_id;
    std::string connection;
    std::string usb;

    [[nodiscard]] bool ready() const noexcept { return state == "device"; }
};

// Parses the stable, machine-oriented output from `adb devices -l`. Exposed so
// clients can rely on the same interpretation without starting a sync session.
[[nodiscard]] std::vector<AdbDevice> parse_adb_devices(std::string_view output);

class Adb {
public:
    Adb(std::optional<std::filesystem::path> requested_path, std::optional<std::string> requested_serial);
    [[nodiscard]] static std::vector<AdbDevice> enumerate(
        const std::optional<std::filesystem::path>& requested_path = std::nullopt);
    [[nodiscard]] const std::string& serial() const noexcept { return serial_; }
    [[nodiscard]] std::string deploy_server();
    [[nodiscard]] unsigned create_forward(std::string_view socket_name);
    void remove_forward(unsigned port) noexcept;
    [[nodiscard]] ChildProcess launch_server(std::string_view remote_path, std::string_view socket_name,
                                             std::string_view token);
private:
    [[nodiscard]] ProcessResult run(const std::vector<std::string>& arguments, bool device = true) const;
    [[nodiscard]] std::string query_abi() const;
    [[nodiscard]] static std::filesystem::path locate(const std::optional<std::filesystem::path>& requested);
    [[nodiscard]] std::string select_serial(const std::optional<std::string>& requested) const;

    std::filesystem::path executable_;
    std::string serial_;
};

}
