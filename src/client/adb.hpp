#pragma once

#include "process_windows.hpp"

#include <filesystem>
#include <optional>

namespace syncer::client {

class Adb {
public:
    Adb(std::optional<std::filesystem::path> requested_path, std::optional<std::string> requested_serial);
    [[nodiscard]] const std::string& serial() const noexcept { return serial_; }
    [[nodiscard]] std::string deploy_server();
    [[nodiscard]] unsigned create_forward(std::string_view socket_name);
    void remove_forward(unsigned port) noexcept;
    [[nodiscard]] ChildProcess launch_server(std::string_view remote_path, std::string_view socket_name,
                                             std::string_view token);
private:
    [[nodiscard]] ProcessResult run(const std::vector<std::wstring>& arguments, bool device = true) const;
    [[nodiscard]] std::string query_abi() const;
    [[nodiscard]] static std::filesystem::path locate(const std::optional<std::filesystem::path>& requested);
    [[nodiscard]] std::string select_serial(const std::optional<std::string>& requested) const;

    std::filesystem::path executable_;
    std::string serial_;
};

}
