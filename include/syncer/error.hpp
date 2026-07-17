#pragma once

#include <stdexcept>
#include <string>

namespace syncer {

enum class ExitCode : int {
    success = 0,
    usage = 2,
    adb = 3,
    safety = 4,
    transfer = 5,
    source_changed = 6,
};

class Error final : public std::runtime_error {
public:
    Error(ExitCode code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}
    [[nodiscard]] ExitCode code() const noexcept { return code_; }
private:
    ExitCode code_;
};

}
