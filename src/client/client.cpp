#include "client.hpp"

#include "syncer/blake3.hpp"
#include "syncer/error.hpp"
#include "syncer/version.hpp"

#include <boost/asio/ip/address_v4.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#endif

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

DestinationManifest ClientSession::scan(std::string_view device_path) {
    ByteWriter writer; writer.string(device_path);
    const auto request = next_request();
    send_frame(socket_, Frame{MessageType::scan, request, std::move(writer).take()});
    DestinationManifest result;
    auto& manifest = result.entries;
    manifest.reserve(4096);
    while (true) {
        const auto response = receive_frame(socket_);
        if (response.type == MessageType::error) {
            ByteReader error(response.payload); const auto code = static_cast<ExitCode>(error.u32()); const auto message = error.string();
            throw Error(code, message);
        }
        if (response.request_id != request) throw Error(ExitCode::transfer, "destination manifest response has wrong request ID");
        if (response.type == MessageType::manifest_end) break;
        ByteReader entry_reader(response.payload);
        const auto add_entry = [&](ScanEntry scanned) {
            if ((scanned.flags & kScanEntryUntrackedTime) != 0)
                result.untracked_times.insert(scanned.entry.path);
            if (!manifest.emplace(scanned.entry.path, std::move(scanned.entry)).second)
                throw Error(ExitCode::transfer, "duplicate destination manifest path");
        };
        if (response.type == MessageType::manifest_entry) {
            add_entry(decode_scan_entry(entry_reader));
        } else if (response.type == MessageType::manifest_batch) {
            const auto count = entry_reader.u32();
            if (count > 100000) throw Error(ExitCode::transfer, "destination manifest batch entry count exceeds limit");
            manifest.reserve(manifest.size() + count);
            for (std::uint32_t index = 0; index < count; ++index) add_entry(decode_scan_entry(entry_reader));
        } else {
            throw Error(ExitCode::transfer, "invalid destination manifest response");
        }
        entry_reader.require_end();
    }
    return result;
}

void ClientSession::select_root(std::string_view device_path) {
    ByteWriter writer;
    writer.string(device_path);
    operation(MessageType::select_root, std::move(writer).take());
}

void ClientSession::validate_plan(const std::vector<DestinationPrecondition>& preconditions) {
    if (preconditions.size() > std::numeric_limits<std::uint32_t>::max())
        throw Error(ExitCode::safety, "saved plan has too many destination preconditions");
    std::size_t begin{};
    while (begin < preconditions.size()) {
        std::size_t end = begin;
        std::size_t encoded_size = 4;
        while (end < preconditions.size()) {
            const auto& value = preconditions[end];
            const auto next_size = 4U + value.path.size() + 1U +
                (value.exists ? 4U + value.entry.path.size() + 1U + 8U + 8U : 0U);
            if (end != begin && encoded_size + next_size > kMaxFramePayload - 1024U) break;
            if (encoded_size + next_size > kMaxFramePayload)
                throw Error(ExitCode::safety, "saved plan contains an oversized destination path");
            encoded_size += next_size;
            ++end;
        }
        ByteWriter writer;
        writer.u32(static_cast<std::uint32_t>(end - begin));
        for (std::size_t index = begin; index < end; ++index) {
            const auto& precondition = preconditions[index];
            writer.string(precondition.path);
            writer.u8(precondition.exists ? 1 : 0);
            if (precondition.exists) encode_manifest_entry(writer, precondition.entry);
        }
        operation(MessageType::validate_plan, std::move(writer).take());
        begin = end;
    }
}

void ClientSession::restore(std::string_view recovery_id) {
    ByteWriter writer;
    writer.string(recovery_id);
    operation(MessageType::restore, std::move(writer).take());
}

void ClientSession::prepare(std::string_view recovery_id) {
    ByteWriter writer;
    writer.string(recovery_id);
    operation(MessageType::prepare, std::move(writer).take());
}

void ClientSession::upload(const SourceFile& source, bool direct) {
    verify_source(source);
    std::ifstream input(source.physical_path, std::ios::binary);
    if (!input) throw Error(ExitCode::source_changed, "cannot open source file: " + source.entry.path);

    ByteWriter begin;
    begin.string(source.entry.path); begin.u64(source.entry.size); begin.i64(source.entry.modified_seconds);
    begin.u8(static_cast<std::uint8_t>(direct ? UploadPlacement::direct : UploadPlacement::staged));
    operation(MessageType::put_begin, std::move(begin).take());

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
void ClientSession::record_times(const std::vector<PlannedChange>& records) {
    std::size_t begin{};
    while (begin < records.size()) {
        std::size_t end = begin;
        std::size_t encoded_size = 4;
        while (end < records.size()) {
            const auto next_size = 4U + records[end].entry.path.size() + 1U + 8U + 8U;
            if (end != begin && encoded_size + next_size > kMaxFramePayload - 1024U) break;
            if (encoded_size + next_size > kMaxFramePayload)
                throw Error(ExitCode::safety, "timestamp record has an oversized destination path");
            encoded_size += next_size;
            ++end;
        }
        ByteWriter writer;
        writer.u32(static_cast<std::uint32_t>(end - begin));
        for (std::size_t index = begin; index < end; ++index) encode_manifest_entry(writer, records[index].entry);
        operation(MessageType::record_times, std::move(writer).take());
        begin = end;
    }
}
void ClientSession::cancel() noexcept {
    if (!socket_.is_open()) return;
#ifdef _WIN32
    (void)::shutdown(socket_.native_handle(), SD_BOTH);
#else
    (void)::shutdown(socket_.native_handle(), SHUT_RDWR);
#endif
}
void ClientSession::shutdown() noexcept {
    if (!socket_.is_open()) return;
    try { operation(MessageType::shutdown); } catch (...) {}
}

} // namespace syncer::client
