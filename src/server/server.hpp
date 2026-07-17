#pragma once

#include "syncer/blake3.hpp"
#include "syncer/error.hpp"
#include "syncer/transport.hpp"

#include <boost/asio/local/stream_protocol.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace syncer::server {

class Server {
public:
    Server(std::string socket_name, std::string token);
    int run();
private:
    using LocalSocket = boost::asio::local::stream_protocol::socket;
    struct Upload {
        std::string path;
        std::filesystem::path temporary_path;
        std::ofstream stream;
        Blake3 hash;
        std::uint64_t expected_size{};
        std::uint64_t received_size{};
        std::int64_t modified_seconds{};
    };
    struct StagedFile {
        std::filesystem::path temporary_path;
        std::uint64_t size{};
        std::int64_t modified_seconds{};
    };
    struct LogicalTime {
        EntryType type{};
        std::uint64_t size{};
        std::int64_t modified_seconds{};
    };

    void serve(LocalSocket& client);
    void dispatch(LocalSocket& client, const Frame& frame);
    void send_ok(LocalSocket& client, std::uint32_t request_id);
    void send_error(LocalSocket& client, std::uint32_t request_id, ExitCode code, std::string_view message);
    void initialize_root(ByteReader& reader);
    void select_managed_root(ByteReader& reader);
    void prepare_mutation();
    void send_manifest(LocalSocket& client, std::uint32_t request_id);
    void make_directory(ByteReader& reader);
    void put_begin(ByteReader& reader);
    void put_chunk(const Frame& frame);
    void put_end(ByteReader& reader);
    void commit_file(ByteReader& reader);
    void delete_path(ByteReader& reader);
    void set_mtime(ByteReader& reader);

    [[nodiscard]] std::filesystem::path validate_root(std::string_view requested, bool allow_missing) const;
    [[nodiscard]] std::filesystem::path destination_path(std::string_view relative, bool ensure_parents);
    void acquire_lock();
    void require_root() const;
    void load_metadata();
    void save_metadata();

    std::string socket_name_;
    std::string token_;
    std::filesystem::path root_;
    int lock_fd_{-1};
    std::uint64_t upload_counter_{};
    std::unique_ptr<Upload> upload_;
    std::map<std::string, StagedFile, std::less<>> staged_;
    std::map<std::string, LogicalTime, std::less<>> logical_times_;
    bool metadata_dirty_{};
    bool stop_{};
};

}
