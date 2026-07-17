#include "client.hpp"

#include "syncer/blake3.hpp"
#include "syncer/error.hpp"
#include "syncer/version.hpp"

#include <boost/asio/ip/address_v4.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace syncer::client {
namespace fs = std::filesystem;
namespace {

bool connect_once(boost::asio::ip::tcp::socket& socket, unsigned port) {
    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(),
                                                   static_cast<unsigned short>(port));
    boost::system::error_code error;
    socket.connect(endpoint, error);
    return !error;
}

void verify_source(const SourceFile& source) {
    std::error_code error;
    const auto size = fs::file_size(source.physical_path, error);
    if (error || size != source.entry.size || source_modified_seconds(source.physical_path) != source.entry.modified_seconds)
        throw Error(ExitCode::source_changed, "source changed during synchronization: " + source.entry.path);
}

} // namespace

ClientSession::ClientSession(unsigned port, std::string token) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!connect_once(socket_, port)) {
            socket_.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        try {
            ByteWriter writer;
            writer.string(token); writer.string(kVersion);
            const auto request = next_request();
            send_frame(socket_, Frame{MessageType::hello, request, std::move(writer).take()});
            const auto response = receive_frame(socket_);
            if (response.type == MessageType::error) {
                ByteReader error(response.payload);
                const auto code = static_cast<ExitCode>(error.u32());
                const auto message = error.string();
                throw Error(code, message);
            }
            if (response.type != MessageType::hello_ack || response.request_id != request)
                throw Error(ExitCode::transfer, "invalid Android server handshake response");
            ByteReader ack(response.payload);
            (void)ack.string(); ack.require_end();
            return;
        } catch (const Error&) {
            socket_.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    throw Error(ExitCode::adb, "timed out connecting to the Android server through ADB");
}

ClientSession::~ClientSession() {
    boost::system::error_code error;
    socket_.close(error);
}

void ClientSession::expect_ok(std::uint32_t request_id) {
    const auto response = receive_frame(socket_);
    if (response.type == MessageType::error) {
        ByteReader reader(response.payload);
        const auto code = static_cast<ExitCode>(reader.u32());
        const auto message = reader.string(); reader.require_end();
        throw Error(code, message);
    }
    if (response.type != MessageType::operation_ok || response.request_id != request_id)
        throw Error(ExitCode::transfer, "unexpected Android server response");
}

void ClientSession::operation(MessageType type, std::vector<std::uint8_t> payload) {
    const auto request = next_request();
    send_frame(socket_, Frame{type, request, std::move(payload)});
    expect_ok(request);
}

void ClientSession::initialize(std::string_view device_path, bool adopt) {
    ByteWriter writer; writer.string(device_path); writer.u8(adopt ? 1 : 0);
    operation(MessageType::initialize, std::move(writer).take());
}

Manifest ClientSession::scan(std::string_view device_path) {
    ByteWriter writer; writer.string(device_path);
    const auto request = next_request();
    send_frame(socket_, Frame{MessageType::scan, request, std::move(writer).take()});
    Manifest manifest;
    while (true) {
        const auto response = receive_frame(socket_);
        if (response.type == MessageType::error) {
            ByteReader error(response.payload); const auto code = static_cast<ExitCode>(error.u32()); const auto message = error.string();
            throw Error(code, message);
        }
        if (response.request_id != request) throw Error(ExitCode::transfer, "destination manifest response has wrong request ID");
        if (response.type == MessageType::manifest_end) break;
        ByteReader entry_reader(response.payload);
        const auto add_entry = [&](ManifestEntry entry) {
            if (!manifest.emplace(entry.path, entry).second)
                throw Error(ExitCode::transfer, "duplicate destination manifest path");
        };
        if (response.type == MessageType::manifest_entry) {
            add_entry(decode_manifest_entry(entry_reader));
        } else if (response.type == MessageType::manifest_batch) {
            const auto count = entry_reader.u32();
            if (count > 100000) throw Error(ExitCode::transfer, "destination manifest batch entry count exceeds limit");
            for (std::uint32_t index = 0; index < count; ++index) add_entry(decode_manifest_entry(entry_reader));
        } else {
            throw Error(ExitCode::transfer, "invalid destination manifest response");
        }
        entry_reader.require_end();
    }
    return manifest;
}

void ClientSession::prepare() { operation(MessageType::prepare); }

void ClientSession::stage(const SourceFile& source) {
    verify_source(source);
    ByteWriter begin;
    begin.string(source.entry.path); begin.u64(source.entry.size); begin.i64(source.entry.modified_seconds);
    operation(MessageType::put_begin, std::move(begin).take());

    std::ifstream input(source.physical_path, std::ios::binary);
    if (!input) throw Error(ExitCode::source_changed, "cannot open source file: " + source.entry.path);
    Blake3 hash;
    std::vector<std::uint8_t> buffer(kTransferChunkSize);
    std::uint64_t total{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) break;
        const std::span chunk(buffer.data(), count);
        hash.update(chunk); total += count;
        send_frame(socket_, Frame{MessageType::put_chunk, next_request(), {chunk.begin(), chunk.end()}});
    }
    if (input.bad() || total != source.entry.size) throw Error(ExitCode::source_changed, "source changed while reading: " + source.entry.path);
    const auto digest = hash.finalize();
    ByteWriter end; end.bytes(digest);
    const auto request = next_request();
    send_frame(socket_, Frame{MessageType::put_end, request, std::move(end).take()});
    expect_ok(request);
    verify_source(source);
}

void ClientSession::make_directory(const ManifestEntry& entry) {
    ByteWriter writer; writer.string(entry.path); writer.i64(entry.modified_seconds);
    operation(MessageType::make_directory, std::move(writer).take());
}
void ClientSession::commit_file(std::string_view path) {
    ByteWriter writer; writer.string(path); operation(MessageType::commit_file, std::move(writer).take());
}
void ClientSession::delete_path(std::string_view path) {
    ByteWriter writer; writer.string(path); operation(MessageType::delete_path, std::move(writer).take());
}
void ClientSession::set_mtime(const ManifestEntry& entry) {
    ByteWriter writer; writer.string(entry.path); writer.i64(entry.modified_seconds);
    operation(MessageType::set_mtime, std::move(writer).take());
}
void ClientSession::shutdown() noexcept {
    if (!socket_.is_open()) return;
    try { operation(MessageType::shutdown); } catch (...) {}
}

} // namespace syncer::client
