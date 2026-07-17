#include "syncer/protocol.hpp"

#include "syncer/error.hpp"

#include <limits>

namespace syncer {
namespace {
constexpr std::uint32_t kMagic = 0x53594E43U;
}

void ByteWriter::u8(std::uint8_t value) { data_.push_back(value); }
void ByteWriter::u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>(value >> 8U)); data_.push_back(static_cast<std::uint8_t>(value));
}
void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) data_.push_back(static_cast<std::uint8_t>(value >> shift));
}
void ByteWriter::u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) data_.push_back(static_cast<std::uint8_t>(value >> shift));
}
void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
void ByteWriter::bytes(std::span<const std::uint8_t> value) { data_.insert(data_.end(), value.begin(), value.end()); }
void ByteWriter::string(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) throw Error(ExitCode::transfer, "string too long");
    u32(static_cast<std::uint32_t>(value.size()));
    bytes({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void ByteReader::require(std::size_t length) const {
    if (length > data_.size() - position_) throw Error(ExitCode::transfer, "truncated protocol payload");
}
std::uint8_t ByteReader::u8() { require(1); return data_[position_++]; }
std::uint16_t ByteReader::u16() { require(2); const auto v = (data_[position_] << 8U) | data_[position_ + 1]; position_ += 2; return static_cast<std::uint16_t>(v); }
std::uint32_t ByteReader::u32() {
    require(4); std::uint32_t v{}; for (int i = 0; i < 4; ++i) v = (v << 8U) | data_[position_++]; return v;
}
std::uint64_t ByteReader::u64() {
    require(8); std::uint64_t v{}; for (int i = 0; i < 8; ++i) v = (v << 8U) | data_[position_++]; return v;
}
std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }
std::vector<std::uint8_t> ByteReader::bytes(std::size_t length) {
    require(length); std::vector<std::uint8_t> v(data_.begin() + static_cast<std::ptrdiff_t>(position_), data_.begin() + static_cast<std::ptrdiff_t>(position_ + length)); position_ += length; return v;
}
std::string ByteReader::string() {
    const auto length = u32(); require(length); std::string v(reinterpret_cast<const char*>(data_.data() + position_), length); position_ += length; return v;
}
void ByteReader::require_end() const { if (position_ != data_.size()) throw Error(ExitCode::transfer, "unexpected protocol payload data"); }

std::vector<std::uint8_t> encode_header(const Frame& frame) {
    if (frame.payload.size() > kMaxFramePayload) throw Error(ExitCode::transfer, "protocol frame is too large");
    ByteWriter writer;
    writer.u32(kMagic); writer.u16(kProtocolVersion); writer.u16(static_cast<std::uint16_t>(frame.type));
    writer.u32(frame.request_id); writer.u32(static_cast<std::uint32_t>(frame.payload.size()));
    return std::move(writer).take();
}

Frame decode_frame_header(std::span<const std::uint8_t, 16> header) {
    ByteReader reader(header);
    if (reader.u32() != kMagic) throw Error(ExitCode::transfer, "invalid protocol magic");
    if (reader.u16() != kProtocolVersion) throw Error(ExitCode::transfer, "incompatible protocol version");
    Frame frame;
    frame.type = static_cast<MessageType>(reader.u16()); frame.request_id = reader.u32();
    const auto size = reader.u32();
    if (size > kMaxFramePayload) throw Error(ExitCode::transfer, "protocol frame exceeds limit");
    frame.payload.reserve(size);
    return frame;
}

void encode_manifest_entry(ByteWriter& writer, const ManifestEntry& entry) {
    writer.string(entry.path); writer.u8(static_cast<std::uint8_t>(entry.type)); writer.u64(entry.size); writer.i64(entry.modified_seconds);
}
ManifestEntry decode_manifest_entry(ByteReader& reader) {
    ManifestEntry entry;
    entry.path = reader.string(); entry.type = static_cast<EntryType>(reader.u8()); entry.size = reader.u64(); entry.modified_seconds = reader.i64();
    if (!valid_relative_path(entry.path)) throw Error(ExitCode::transfer, "invalid relative path in manifest");
    if (entry.type != EntryType::file && entry.type != EntryType::directory && entry.type != EntryType::other) throw Error(ExitCode::transfer, "invalid manifest entry type");
    return entry;
}

} // namespace syncer
