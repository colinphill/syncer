#pragma once

#include "scanner.hpp"
#include "syncer/transport.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <string>

namespace syncer::client {

class ClientSession {
public:
    ClientSession(unsigned port, std::string token);
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    ~ClientSession();

    void initialize(std::string_view device_path, bool adopt);
    [[nodiscard]] Manifest scan(std::string_view device_path);
    void prepare();
    void stage(const SourceFile& source);
    void make_directory(const ManifestEntry& entry);
    void commit_file(std::string_view path);
    void delete_path(std::string_view path);
    void set_mtime(const ManifestEntry& entry);
    void shutdown() noexcept;
private:
    [[nodiscard]] std::uint32_t next_request() { return ++request_id_; }
    void operation(MessageType type, std::vector<std::uint8_t> payload = {});
    void expect_ok(std::uint32_t request_id);

    boost::asio::io_context context_;
    boost::asio::ip::tcp::socket socket_{context_};
    std::uint32_t request_id_{};
};

}
