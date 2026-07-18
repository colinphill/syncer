#pragma once

#include "syncer/manifest.hpp"
#include "syncer/version.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace syncer {

enum class MessageType : std::uint16_t {
    hello = 1,
    hello_ack = 2,
    initialize = 3,
    scan = 4,
    manifest_entry = 5,
    manifest_end = 6,
    make_directory = 7,
    put_begin = 8,
    put_chunk = 9,
    put_end = 10,
    commit_file = 11,
    delete_path = 12,
    set_mtime = 13,
    operation_ok = 14,
    error = 15,
    shutdown = 16,
    manifest_batch = 17,
    prepare = 18,
};

enum class UploadPlacement : std::uint8_t { staged = 0, direct = 1 };

struct Frame {
    MessageType type{};
    std::uint32_t request_id{};
    std::vector<std::uint8_t> payload;
};

class ByteWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void bytes(std::span<const std::uint8_t> value);
    void string(std::string_view value);
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(data_); }
private:
    std::vector<std::uint8_t> data_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}
    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] std::int64_t i64();
    [[nodiscard]] std::vector<std::uint8_t> bytes(std::size_t length);
    [[nodiscard]] std::string string();
    void require_end() const;
private:
    void require(std::size_t length) const;
    std::span<const std::uint8_t> data_;
    std::size_t position_{};
};

[[nodiscard]] std::vector<std::uint8_t> encode_header(const Frame& frame);
[[nodiscard]] Frame decode_frame_header(std::span<const std::uint8_t, 16> header);
void encode_manifest_entry(ByteWriter& writer, const ManifestEntry& entry);
[[nodiscard]] ManifestEntry decode_manifest_entry(ByteReader& reader);
[[nodiscard]] UploadPlacement decode_upload_placement(ByteReader& reader);

}
