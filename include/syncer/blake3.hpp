#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace syncer {

using Digest = std::array<std::uint8_t, 32>;

class Blake3 {
public:
    Blake3();
    ~Blake3();
    Blake3(const Blake3&) = delete;
    Blake3& operator=(const Blake3&) = delete;
    Blake3(Blake3&&) = delete;
    Blake3& operator=(Blake3&&) = delete;
    void update(std::span<const std::uint8_t> input);
    [[nodiscard]] Digest finalize() const;
private:
    struct Impl;
    alignas(8) std::array<std::byte, 1904> storage_{};
    Impl* impl();
    const Impl* impl() const;
};

[[nodiscard]] std::string digest_hex(const Digest& digest);

}
