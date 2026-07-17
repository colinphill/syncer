#pragma once

#include <cstddef>
#include <cstdint>

namespace syncer {
inline constexpr const char* kVersion = SYNCER_VERSION;
inline constexpr std::uint16_t kProtocolVersion = SYNCER_PROTOCOL_VERSION;
inline constexpr const char* kReservedDirectory = ".syncer";
inline constexpr std::uint32_t kMaxFramePayload = 2U * 1024U * 1024U;
inline constexpr std::size_t kTransferChunkSize = 1024U * 1024U;
}
