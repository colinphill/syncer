#pragma once

#include "syncer/error.hpp"
#include "syncer/protocol.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>

namespace syncer {

template <typename SyncWriteStream>
void send_frame(SyncWriteStream& stream, const Frame& frame) {
    const auto header = encode_header(frame);
    boost::system::error_code error;
    const std::array buffers{boost::asio::buffer(header), boost::asio::buffer(frame.payload)};
    boost::asio::write(stream, buffers, error);
    if (error) throw Error(ExitCode::transfer, "socket send failed: " + error.message());
}

template <typename SyncReadStream>
[[nodiscard]] Frame receive_frame(SyncReadStream& stream) {
    std::array<std::uint8_t, 16> header{};
    boost::system::error_code error;
    boost::asio::read(stream, boost::asio::buffer(header), error);
    if (error) throw Error(ExitCode::transfer, "socket receive failed: " + error.message());
    auto frame = decode_frame_header(header);
    const auto payload_size = (static_cast<std::uint32_t>(header[12]) << 24U) |
                              (static_cast<std::uint32_t>(header[13]) << 16U) |
                              (static_cast<std::uint32_t>(header[14]) << 8U) | header[15];
    frame.payload.resize(payload_size);
    if (payload_size != 0) {
        boost::asio::read(stream, boost::asio::buffer(frame.payload), error);
        if (error) throw Error(ExitCode::transfer, "socket receive failed: " + error.message());
    }
    return frame;
}

}
