#include "server.hpp"

#include "syncer/error.hpp"
#include "syncer/version.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace syncer::server {
namespace fs = std::filesystem;
namespace {

void apply_mtime(const fs::path& path, std::int64_t seconds) {
    if (seconds < static_cast<std::int64_t>(std::numeric_limits<time_t>::min()) ||
        seconds > static_cast<std::int64_t>(std::numeric_limits<time_t>::max()))
        throw Error(ExitCode::transfer, "destination modification time is outside the platform range");
    const auto value = static_cast<time_t>(seconds);
    const timespec times[2]{{value, 0}, {value, 0}};
    if (::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
        const auto failure = errno;
        // Android's emulated shared-storage filesystem rejects timestamp changes
        // on some releases. The managed logical-time catalog preserves sync semantics.
        if (failure == EPERM || failure == EACCES) return;
        throw Error(ExitCode::transfer, "cannot set destination modification time: " + path.string());
    }
}

bool is_descendant(const fs::path& child, const fs::path& parent) {
    const auto relative = child.lexically_relative(parent);
    return !relative.empty() && relative != "." && !relative.is_absolute() && *relative.begin() != "..";
}

std::string random_id() {
    std::random_device source;
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for (auto& character : result) character = digits[source() & 15U];
    return result;
}

std::string root_lock_id(const fs::path& root) {
    const auto value = root.generic_string();
    Blake3 hash;
    hash.update({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    const auto digest = hash.finalize();
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

bool ascii_iequals(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

std::optional<fs::path> internal_storage_root() {
    std::vector<fs::path> candidates;
    if (const char* configured = std::getenv("EXTERNAL_STORAGE"); configured && *configured)
        candidates.emplace_back(configured);
    candidates.emplace_back("/sdcard");
    candidates.emplace_back("/storage/emulated/0");
    candidates.emplace_back("/storage/self/primary");
    for (const auto& candidate : candidates) {
        std::error_code error;
        const auto resolved = fs::canonical(candidate, error);
        if (!error) return resolved;
    }
    return std::nullopt;
}

bool valid_recovery_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' || character == '_';
        });
}

std::string hex_encode(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

std::string hex_decode(std::string_view value) {
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    if ((value.size() & 1U) != 0) throw Error(ExitCode::safety, "recovery journal has an invalid path");
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const auto high = digit(value[index]);
        const auto low = digit(value[index + 1]);
        if (high < 0 || low < 0) throw Error(ExitCode::safety, "recovery journal has an invalid path");
        result.push_back(static_cast<char>((high << 4) | low));
    }
    if (!valid_relative_path(result)) throw Error(ExitCode::safety, "recovery journal has an unsafe path");
    return result;
}

std::vector<std::string_view> split_fields(const std::string& line) {
    std::vector<std::string_view> result;
    std::string_view remaining(line);
    while (true) {
        const auto separator = remaining.find('\t');
        result.push_back(remaining.substr(0, separator));
        if (separator == remaining.npos) break;
        remaining.remove_prefix(separator + 1);
    }
    return result;
}

template <typename Value>
Value parse_number(std::string_view text) {
    Value value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw Error(ExitCode::safety, "recovery journal has an invalid number");
    return value;
}

std::vector<std::string> topmost_paths(std::vector<std::string> paths) {
    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        const auto left_depth = path_depth(left), right_depth = path_depth(right);
        return left_depth != right_depth ? left_depth < right_depth : left < right;
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    std::vector<std::string> result;
    for (auto& path : paths) {
        bool covered{};
        for (const auto& parent : result) {
            if (path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/') {
                covered = true;
                break;
            }
        }
        if (!covered) result.push_back(std::move(path));
    }
    return result;
}

std::vector<fs::path> removable_storage_roots(const std::optional<fs::path>& internal) {
    std::error_code error;
    const auto storage = fs::canonical("/storage", error);
    if (error) throw Error(ExitCode::safety, "cannot resolve Android storage mount directory");
    std::vector<fs::path> roots;
    fs::directory_iterator entries(storage, error), end;
    if (error) throw Error(ExitCode::safety, "cannot inspect Android storage volumes");
    for (; entries != end; entries.increment(error)) {
        if (error) throw Error(ExitCode::safety, "cannot inspect Android storage volumes");
        const auto name = entries->path().filename().generic_string();
        if (ascii_iequals(name, "emulated") || ascii_iequals(name, "self")) continue;
        const auto status = entries->status(error);
        if (error || !fs::is_directory(status)) continue;
        const auto root = fs::canonical(entries->path(), error);
        if (error) { error.clear(); continue; }
        if ((internal && root == *internal) || !is_descendant(root, storage)) continue;
        roots.push_back(root);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

} // namespace

Server::Server(std::string socket_name, std::string token)
    : socket_name_(std::move(socket_name)), token_(std::move(token)) {}

int Server::run() {
    ::signal(SIGPIPE, SIG_IGN);
    if (socket_name_.size() > 90) throw Error(ExitCode::usage, "socket name is too long");
    boost::asio::io_context context;
    boost::asio::local::stream_protocol::acceptor acceptor(context);
    boost::system::error_code error;
    acceptor.open(boost::asio::local::stream_protocol(), error);
    if (error) throw Error(ExitCode::transfer, "cannot create server socket: " + error.message());
    std::string abstract_name(1, '\0');
    abstract_name += socket_name_;
    acceptor.bind(boost::asio::local::stream_protocol::endpoint(abstract_name), error);
    if (!error) acceptor.listen(1, error);
    if (error) throw Error(ExitCode::transfer, "cannot bind server socket: " + error.message());
    LocalSocket client(context);
    acceptor.accept(client, error);
    acceptor.close();
    if (error) throw Error(ExitCode::transfer, "cannot accept client connection: " + error.message());
    const timeval timeout{30, 0};
    ::setsockopt(client.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    serve(client);
    client.close(error);
    if (lock_fd_ >= 0) ::close(lock_fd_);
    return 0;
}

void Server::serve(LocalSocket& client) {
    const auto hello = receive_frame(client);
    if (hello.type != MessageType::hello) throw Error(ExitCode::transfer, "client did not send handshake");
    ByteReader reader(hello.payload);
    const auto supplied_token = reader.string();
    (void)reader.string();
    reader.require_end();
    if (supplied_token.size() != token_.size() || !std::equal(supplied_token.begin(), supplied_token.end(), token_.begin()))
        throw Error(ExitCode::transfer, "session authentication failed");
    ByteWriter ack;
    ack.string(kVersion);
    send_frame(client, Frame{MessageType::hello_ack, hello.request_id, std::move(ack).take()});

    while (!stop_) {
        Frame frame;
        try {
            frame = receive_frame(client);
        } catch (const Error&) {
            // A client-side preflight or transfer failure closes the command-scoped
            // connection. Cleanup and release the root lock without noisy stderr.
            discard_upload();
            return;
        }
        try {
            dispatch(client, frame);
        } catch (const Error& error) {
            discard_upload();
            send_error(client, frame.request_id, error.code(), error.what());
        } catch (const std::exception& error) {
            discard_upload();
            send_error(client, frame.request_id, ExitCode::transfer, error.what());
        }
    }
}

void Server::dispatch(LocalSocket& client, const Frame& frame) {
    ByteReader reader(frame.payload);
    switch (frame.type) {
    case MessageType::initialize: initialize_root(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::scan: select_managed_root(reader); reader.require_end(); send_manifest(client, frame.request_id); break;
    case MessageType::select_root: select_managed_root(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::validate_plan: validate_plan(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::restore: restore_recovery(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::prepare:
        {
            const auto recovery_id = reader.string();
            reader.require_end();
            if (!valid_recovery_id(recovery_id))
                throw Error(ExitCode::safety, "invalid recovery identifier");
            recovery_root_ = root_ / kReservedDirectory / "recovery" / recovery_id / "files";
            recovery_journal_ = recovery_root_.parent_path() / "journal.tsv";
            prepare_mutation();
            send_ok(client, frame.request_id);
            break;
        }
    case MessageType::make_directory: make_directory(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::put_begin: put_begin(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::put_chunk: put_chunk(frame); break;
    case MessageType::put_end: put_end(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::commit_file: commit_file(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::delete_path: delete_path(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::set_mtime: set_mtime(reader); reader.require_end(); send_ok(client, frame.request_id); break;
    case MessageType::shutdown:
        reader.require_end();
        save_metadata();
        if (!recovery_root_.empty()) append_recovery_journal("COMMIT", "-");
        stop_ = true;
        send_ok(client, frame.request_id);
        break;
    default: throw Error(ExitCode::transfer, "unsupported protocol message");
    }
}

void Server::send_ok(LocalSocket& client, std::uint32_t request_id) {
    send_frame(client, Frame{MessageType::operation_ok, request_id, {}});
}

void Server::send_error(LocalSocket& client, std::uint32_t request_id, ExitCode code, std::string_view message) {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(code)); writer.string(message);
    send_frame(client, Frame{MessageType::error, request_id, std::move(writer).take()});
}

fs::path Server::validate_root(std::string_view requested, bool allow_missing) const {
    const auto internal = internal_storage_root();
    std::error_code error;
    const auto removable = removable_storage_roots(internal);
    fs::path raw(requested);
    if (!raw.is_absolute()) {
        if (raw.has_parent_path() || !ascii_iequals(raw.generic_string(), "music"))
            throw Error(ExitCode::safety, "Android destination must be absolute or the music alias");
        std::optional<fs::path> match;
        for (const auto& volume : removable) {
            fs::directory_iterator entries(volume, error), end;
            if (error) throw Error(ExitCode::safety, "cannot inspect removable SD card contents");
            for (; entries != end; entries.increment(error)) {
                if (error) throw Error(ExitCode::safety, "cannot inspect removable SD card contents");
                const auto name = entries->path().filename().generic_string();
                if (!ascii_iequals(name, "music")) continue;
                const auto status = entries->status(error);
                if (error || !fs::is_directory(status))
                    throw Error(ExitCode::safety, "the case-insensitive music entry on the removable SD card is not a folder");
                if (match) throw Error(ExitCode::safety, "multiple case-insensitive music folders exist on removable storage");
                match = entries->path();
            }
        }
        if (match) raw = *match;
        else if (removable.empty()) throw Error(ExitCode::safety, "no mounted removable SD card was found");
        else if (allow_missing && removable.size() == 1) raw = removable.front() / "Music";
        else if (allow_missing) throw Error(ExitCode::safety, "multiple removable storage volumes exist; use an absolute DEVICE_PATH");
        else throw Error(ExitCode::safety, "the removable SD card has no music folder");
    }
    auto candidate = allow_missing ? fs::weakly_canonical(raw, error) : fs::canonical(raw, error);
    if (error) throw Error(ExitCode::safety, "cannot resolve Android destination");
    bool allowed = internal && is_descendant(candidate, *internal);
    for (const auto& root : removable) allowed = allowed || is_descendant(candidate, root);
    if (!allowed) throw Error(ExitCode::safety, "destination must be below internal shared storage or a mounted removable SD card");
    return candidate;
}

void Server::initialize_root(ByteReader& reader) {
    const auto requested = reader.string();
    const bool adopt = reader.u8() != 0;
    root_ = validate_root(requested, true);
    std::error_code error;
    const bool existed = fs::exists(root_, error);
    if (error) throw Error(ExitCode::safety, "cannot inspect destination root");
    if (!existed) fs::create_directories(root_);
    if (!fs::is_directory(root_)) throw Error(ExitCode::safety, "destination root is not a directory");
    const auto state = root_ / kReservedDirectory;
    const auto marker = state / "marker";
    auto validate_marker = [&] {
        std::ifstream input(marker, std::ios::binary);
        std::string signature, version;
        std::getline(input, signature); std::getline(input, version);
        if (signature != "SYNCER" || version != "1")
            throw Error(ExitCode::safety, "destination has an invalid syncer marker");
    };
    if (fs::exists(marker)) {
        validate_marker();
        fs::create_directories(state / "tmp");
        acquire_lock();
        load_metadata();
        return;
    }
    bool nonempty = false;
    for (const auto& ignored : fs::directory_iterator(root_)) { (void)ignored; nonempty = true; break; }
    if (nonempty && !adopt) throw Error(ExitCode::safety, "destination is non-empty; use --adopt to initialize it");
    fs::create_directories(state / "tmp");
    acquire_lock();
    if (fs::exists(marker)) { validate_marker(); return; }
    const auto temporary = state / "marker.new";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "SYNCER\n1\n" << random_id() << '\n';
        if (!output) throw Error(ExitCode::safety, "cannot write destination marker");
    }
    fs::rename(temporary, marker);
    logical_times_.clear();
    metadata_dirty_ = true;
}

void Server::select_managed_root(ByteReader& reader) {
    root_ = validate_root(reader.string(), false);
    const auto marker = root_ / kReservedDirectory / "marker";
    std::ifstream input(marker, std::ios::binary);
    std::string signature;
    std::getline(input, signature);
    std::string version;
    std::getline(input, version);
    if (signature != "SYNCER" || version != "1") throw Error(ExitCode::safety, "destination is not an initialized syncer root");
    acquire_lock();
    load_metadata();
}

void Server::validate_plan(ByteReader& reader) {
    require_root();
    const auto count = reader.u32();
    if (count > 1000000U)
        throw Error(ExitCode::safety, "saved plan has too many destination preconditions");
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto relative = reader.string();
        const bool expected_exists = reader.u8() != 0;
        std::optional<ManifestEntry> expected;
        if (expected_exists) {
            expected = decode_manifest_entry(reader);
            if (expected->path != relative)
                throw Error(ExitCode::safety, "saved plan has an invalid destination precondition");
        }
        if (!valid_relative_path(relative) || relative == kReservedDirectory ||
            relative.starts_with(std::string(kReservedDirectory) + "/"))
            throw Error(ExitCode::safety, "saved plan has an invalid destination path");

        const auto path = destination_path(relative, false);
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw Error(ExitCode::transfer, "cannot inspect saved-plan destination path");
        const bool exists = !error && fs::exists(status);
        if (exists != expected_exists)
            throw Error(ExitCode::source_changed,
                        "destination changed after the plan was saved: " + relative);
        if (!exists) continue;

        ManifestEntry actual{relative, EntryType::other, 0, 0};
        if (fs::is_regular_file(status)) {
            actual.type = EntryType::file;
            actual.size = fs::file_size(path, error);
            if (error) throw Error(ExitCode::transfer, "cannot inspect saved-plan destination file");
            const auto logical = logical_times_.find(relative);
            if (logical != logical_times_.end() && logical->second.type == EntryType::file &&
                logical->second.size == actual.size) {
                actual.modified_seconds = logical->second.modified_seconds;
            } else {
                struct stat value{};
                if (::lstat(path.c_str(), &value) != 0)
                    throw Error(ExitCode::transfer, "cannot inspect saved-plan destination file");
                actual.modified_seconds = static_cast<std::int64_t>(value.st_mtim.tv_sec);
            }
        } else if (fs::is_directory(status)) {
            actual.type = EntryType::directory;
        }
        if (actual != *expected)
            throw Error(ExitCode::source_changed,
                        "destination changed after the plan was saved: " + relative);
    }
}

void Server::prepare_mutation() {
    require_root();
    const auto temporary = root_ / kReservedDirectory / "tmp";
    std::error_code error;
    fs::remove_all(temporary, error);
    if (error) throw Error(ExitCode::transfer, "cannot clear stale temporary files");
    fs::create_directories(temporary);
    if (recovery_root_.empty()) throw Error(ExitCode::safety, "no recovery run was selected");
    if (fs::exists(recovery_root_.parent_path()))
        throw Error(ExitCode::safety, "recovery identifier is already in use");
    fs::create_directories(recovery_root_);
    append_recovery_journal("FORMAT", "2");
    append_recovery_journal("BEGIN", "-");
}

void Server::append_recovery_journal(std::string_view action, std::string_view relative) {
    std::ofstream output(recovery_journal_, std::ios::binary | std::ios::app);
    output << action << '\t' << relative << '\n';
    if (!output) throw Error(ExitCode::transfer, "cannot write recovery journal");
}

void Server::append_recovery_entry(std::string_view action, const ManifestEntry& entry) {
    std::ofstream output(recovery_journal_, std::ios::binary | std::ios::app);
    output << action << '\t' << hex_encode(entry.path) << '\t'
           << static_cast<unsigned>(entry.type) << '\t' << entry.size << '\t'
           << entry.modified_seconds << '\n';
    if (!output) throw Error(ExitCode::transfer, "cannot write recovery journal");
}

void Server::restore_recovery(ByteReader& reader) {
    require_root();
    const auto recovery_id = reader.string();
    if (!valid_recovery_id(recovery_id)) throw Error(ExitCode::safety, "invalid recovery identifier");
    const auto run_root = root_ / kReservedDirectory / "recovery" / recovery_id;
    const auto files_root = run_root / "files";
    const auto displaced_root = run_root / "displaced";
    const auto journal = run_root / "journal.tsv";
    std::ifstream input(journal, std::ios::binary);
    if (!input) throw Error(ExitCode::safety, "recovery run was not found");

    std::vector<ManifestEntry> quarantined, installed, logical;
    bool format{}, began{}, committed{}, restore_started{}, restored{};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto fields = split_fields(line);
        if (fields.size() == 2 && fields[0] == "FORMAT") {
            format = fields[1] == "2";
        } else if (fields.size() == 2 && fields[0] == "BEGIN") {
            began = true;
        } else if (fields.size() == 2 && fields[0] == "COMMIT") {
            committed = true;
        } else if (fields.size() == 2 && fields[0] == "RESTORE_BEGIN") {
            restore_started = true;
        } else if (fields.size() == 2 && fields[0] == "RESTORE_COMMIT") {
            restored = true;
        } else if (fields.size() == 5 &&
                   (fields[0] == "QUARANTINE" || fields[0] == "INSTALL" || fields[0] == "LOGICAL")) {
            ManifestEntry entry;
            entry.path = hex_decode(fields[1]);
            const auto type = parse_number<unsigned>(fields[2]);
            if (type < static_cast<unsigned>(EntryType::file) ||
                type > static_cast<unsigned>(EntryType::other))
                throw Error(ExitCode::safety, "recovery journal has an invalid entry type");
            entry.type = static_cast<EntryType>(type);
            entry.size = parse_number<std::uint64_t>(fields[3]);
            entry.modified_seconds = parse_number<std::int64_t>(fields[4]);
            if (fields[0] == "QUARANTINE") quarantined.push_back(std::move(entry));
            else if (fields[0] == "INSTALL") installed.push_back(std::move(entry));
            else logical.push_back(std::move(entry));
        } else {
            throw Error(ExitCode::safety, "recovery journal is invalid or predates one-click restore");
        }
    }
    if (!input.eof()) throw Error(ExitCode::transfer, "cannot read recovery journal");
    if (!format || !began || !committed)
        throw Error(ExitCode::safety, "recovery run is incomplete or predates one-click restore");
    if (restored) throw Error(ExitCode::safety, "recovery run has already been restored");
    if (restore_started) throw Error(ExitCode::safety, "a previous restore of this run was interrupted");
    if (fs::exists(displaced_root)) throw Error(ExitCode::safety, "recovery run already contains displaced restore data");

    std::vector<std::string> affected;
    affected.reserve(quarantined.size() + installed.size());
    for (const auto& entry : quarantined) affected.push_back(entry.path);
    for (const auto& entry : installed) affected.push_back(entry.path);
    affected = topmost_paths(std::move(affected));
    std::vector<std::string> backups;
    backups.reserve(quarantined.size());
    for (const auto& entry : quarantined) backups.push_back(entry.path);
    backups = topmost_paths(std::move(backups));

    for (const auto& relative : backups) {
        std::error_code error;
        const auto status = fs::symlink_status(files_root / fs::path(relative), error);
        if (error || !fs::exists(status))
            throw Error(ExitCode::safety, "recovery data is missing: " + relative);
    }
    // Inspect every destination before the first mutation so unsafe parent changes cannot
    // leave a partially restored run.
    for (const auto& relative : affected) (void)destination_path(relative, false);

    recovery_journal_ = journal;
    append_recovery_journal("RESTORE_BEGIN", "-");
    fs::create_directories(displaced_root);
    std::vector<std::pair<fs::path, fs::path>> displaced;
    std::vector<std::pair<fs::path, fs::path>> restored_backups;
    try {
        for (const auto& relative : affected) {
            const auto destination = destination_path(relative, false);
            std::error_code error;
            const auto status = fs::symlink_status(destination, error);
            if (error && error != std::errc::no_such_file_or_directory)
                throw Error(ExitCode::transfer, "cannot inspect path before restore: " + relative);
            if (error || !fs::exists(status)) continue;
            const auto displaced_path = displaced_root / fs::path(relative);
            fs::create_directories(displaced_path.parent_path());
            fs::rename(destination, displaced_path, error);
            if (error) throw Error(ExitCode::transfer, "cannot preserve current path before restore: " + relative);
            displaced.emplace_back(displaced_path, destination);
        }
        for (const auto& relative : backups) {
            const auto source = files_root / fs::path(relative);
            const auto destination = destination_path(relative, true);
            std::error_code error;
            fs::rename(source, destination, error);
            if (error) throw Error(ExitCode::transfer, "cannot restore quarantined path: " + relative);
            restored_backups.emplace_back(destination, source);
        }
    } catch (...) {
        std::error_code ignored;
        for (auto iterator = restored_backups.rbegin(); iterator != restored_backups.rend(); ++iterator) {
            fs::create_directories(iterator->second.parent_path(), ignored);
            fs::rename(iterator->first, iterator->second, ignored);
        }
        for (auto iterator = displaced.rbegin(); iterator != displaced.rend(); ++iterator) {
            fs::create_directories(iterator->second.parent_path(), ignored);
            fs::rename(iterator->first, iterator->second, ignored);
        }
        throw;
    }

    for (const auto& relative : affected) {
        const auto prefix = relative + "/";
        for (auto iterator = logical_times_.begin(); iterator != logical_times_.end();) {
            if (iterator->first == relative || iterator->first.starts_with(prefix)) iterator = logical_times_.erase(iterator);
            else ++iterator;
        }
    }
    for (const auto& entry : logical) {
        std::error_code error;
        if (fs::exists(root_ / fs::path(entry.path), error))
            logical_times_[entry.path] = LogicalTime{entry.type, entry.size, entry.modified_seconds};
    }
    metadata_dirty_ = true;
    append_recovery_journal("RESTORE_COMMIT", "-");
}

void Server::acquire_lock() {
    if (lock_fd_ >= 0) return;
    std::error_code error;
    const fs::path lock_directory = "/data/local/tmp/syncer/locks";
    fs::create_directories(lock_directory, error);
    if (error) throw Error(ExitCode::safety, "cannot create Android lock directory");
    const auto lock_path = lock_directory / (root_lock_id(root_) + ".lock");
    lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) throw Error(ExitCode::safety, "cannot open destination lock");
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        const auto failure = errno;
        if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
        if (failure != EWOULDBLOCK && failure != EAGAIN)
            throw Error(ExitCode::safety, "cannot lock destination on this Android device");
        throw Error(ExitCode::safety, "destination is locked by another sync operation");
    }
    // Keep a visible lock marker in the managed root; the actual advisory lock
    // lives on /data because recent emulated shared-storage filesystems reject flock.
    std::ofstream visible(root_ / kReservedDirectory / "lock", std::ios::app);
    if (!visible) throw Error(ExitCode::safety, "cannot create destination lock marker");
}

void Server::require_root() const {
    if (root_.empty() || lock_fd_ < 0) throw Error(ExitCode::safety, "no managed destination was selected");
}

void Server::load_metadata() {
    require_root();
    logical_times_.clear();
    metadata_dirty_ = false;
    const auto path = root_ / kReservedDirectory / "timestamps";
    std::error_code error;
    if (!fs::exists(path, error)) return;
    const auto size = fs::file_size(path, error);
    constexpr std::uint64_t maximum_size = 64ULL * 1024ULL * 1024ULL;
    if (error || size > maximum_size) throw Error(ExitCode::safety, "destination timestamp catalog is invalid");
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!input && !data.empty()) throw Error(ExitCode::safety, "cannot read destination timestamp catalog");
    ByteReader reader(data);
    if (reader.string() != "SYNCER-TIMESTAMPS" || reader.u32() != 1)
        throw Error(ExitCode::safety, "destination timestamp catalog has an unsupported format");
    const auto count = reader.u32();
    constexpr std::size_t minimum_catalog_entry_size = 4U + 1U + 1U + 8U + 8U;
    if (count > maximum_size / minimum_catalog_entry_size)
        throw Error(ExitCode::safety, "destination timestamp catalog entry count exceeds limit");
    logical_times_.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto relative = reader.string();
        const auto type = static_cast<EntryType>(reader.u8());
        const auto entry_size = reader.u64();
        const auto seconds = reader.i64();
        if (!valid_relative_path(relative) || relative == kReservedDirectory ||
            relative.starts_with(std::string(kReservedDirectory) + "/") ||
            (type != EntryType::file && type != EntryType::directory) ||
            !logical_times_.emplace(std::move(relative), LogicalTime{type, entry_size, seconds}).second)
            throw Error(ExitCode::safety, "destination timestamp catalog contains an invalid entry");
    }
    reader.require_end();
    for (auto iterator = logical_times_.begin(); iterator != logical_times_.end();) {
        if (iterator->second.type == EntryType::directory) {
            iterator = logical_times_.erase(iterator);
            metadata_dirty_ = true;
        } else {
            ++iterator;
        }
    }
}

void Server::save_metadata() {
    if (!metadata_dirty_) return;
    require_root();
    if (logical_times_.size() > std::numeric_limits<std::uint32_t>::max())
        throw Error(ExitCode::transfer, "destination timestamp catalog is too large");
    ByteWriter writer;
    writer.string("SYNCER-TIMESTAMPS");
    writer.u32(1);
    writer.u32(static_cast<std::uint32_t>(logical_times_.size()));
    for (const auto& [relative, entry] : logical_times_) {
        writer.string(relative);
        writer.u8(static_cast<std::uint8_t>(entry.type));
        writer.u64(entry.size);
        writer.i64(entry.modified_seconds);
    }
    const auto data = std::move(writer).take();
    constexpr std::size_t maximum_size = 64ULL * 1024ULL * 1024ULL;
    if (data.size() > maximum_size) throw Error(ExitCode::transfer, "destination timestamp catalog is too large");
    const auto state = root_ / kReservedDirectory;
    const auto temporary = state / "timestamps.new";
    const auto destination = state / "timestamps";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!output) throw Error(ExitCode::transfer, "cannot write destination timestamp catalog");
    }
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) throw Error(ExitCode::transfer, "cannot commit destination timestamp catalog");
    metadata_dirty_ = false;
}

fs::path Server::destination_path(std::string_view relative, bool ensure_parents) {
    require_root();
    if (!valid_relative_path(relative) || relative == kReservedDirectory || relative.starts_with(std::string(kReservedDirectory) + "/"))
        throw Error(ExitCode::safety, "invalid destination-relative path");
    const fs::path relative_path(relative);
    fs::path current = root_;
    for (const auto& component : relative_path.parent_path()) {
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error && error != std::errc::no_such_file_or_directory) throw Error(ExitCode::safety, "cannot inspect destination parent");
        if (fs::is_symlink(status)) throw Error(ExitCode::safety, "destination parent contains a symbolic link");
        if (fs::exists(status) && !fs::is_directory(status)) {
            if (!ensure_parents) throw Error(ExitCode::safety, "destination parent is not a directory");
            fs::remove_all(current);
        }
        if (ensure_parents && !fs::exists(current)) fs::create_directory(current);
    }
    return root_ / relative_path;
}

void Server::send_manifest(LocalSocket& client, std::uint32_t request_id) {
    require_root();
    constexpr std::size_t batch_limit = 512U * 1024U;
    constexpr std::size_t worker_batch_entries = 256;
    constexpr std::size_t queued_batch_limit = 8;
    std::vector<ManifestEntry> batch;
    std::size_t batch_bytes{};
    const auto flush = [&] {
        if (batch.empty()) return;
        ByteWriter writer;
        writer.u32(static_cast<std::uint32_t>(batch.size()));
        for (const auto& entry : batch) encode_manifest_entry(writer, entry);
        send_frame(client, Frame{MessageType::manifest_batch, request_id, std::move(writer).take()});
        batch.clear();
        batch_bytes = 0;
    };
    const auto emit = [&](ManifestEntry entry) {
        const auto encoded_size = 4U + entry.path.size() + 1U + 8U + 8U;
        if (!batch.empty() && batch_bytes + encoded_size > batch_limit) flush();
        batch_bytes += encoded_size;
        batch.push_back(std::move(entry));
    };

    struct DirectoryTask {
        fs::path physical;
        std::string relative;
        std::size_t depth{};
    };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<DirectoryTask> directories;
    std::deque<std::vector<ManifestEntry>> completed_batches;
    directories.push_back({root_, {}, 0});
    std::size_t outstanding_directories = 1;
    bool finished{};
    bool stopping{};
    std::exception_ptr failure;

    const auto fail = [&](std::exception_ptr error) {
        std::lock_guard lock(mutex);
        if (!failure) failure = std::move(error);
        stopping = true;
        directories.clear();
        ready.notify_all();
    };
    const auto publish = [&](std::vector<ManifestEntry>& entries) {
        if (entries.empty()) return true;
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping || completed_batches.size() < queued_batch_limit; });
        if (stopping) return false;
        completed_batches.push_back(std::move(entries));
        entries.clear();
        entries.reserve(worker_batch_entries);
        ready.notify_all();
        return true;
    };
    const auto worker = [&] {
        while (true) {
            DirectoryTask task;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || !directories.empty(); });
                if (stopping) return;
                task = std::move(directories.front());
                directories.pop_front();
            }
            try {
                if (task.depth > 1024)
                    throw Error(ExitCode::transfer, "destination directory nesting exceeds limit");
                std::unique_ptr<DIR, int(*)(DIR*)> directory(::opendir(task.physical.c_str()), ::closedir);
                if (!directory) throw Error(ExitCode::transfer, "cannot open destination directory");
                std::vector<ManifestEntry> entries;
                entries.reserve(worker_batch_entries);
                while (true) {
                    errno = 0;
                    const auto* native = ::readdir(directory.get());
                    if (!native) {
                        if (errno != 0) throw Error(ExitCode::transfer, "cannot enumerate destination directory");
                        break;
                    }
                    const std::string name(native->d_name);
                    if (name == "." || name == "..") continue;
                    const auto relative = task.relative.empty() ? name : task.relative + "/" + name;
                    if (task.relative.empty() && relative == kReservedDirectory) continue;
                    if (!valid_relative_path(relative))
                        throw Error(ExitCode::safety, "destination contains an invalid path");
                    struct stat value{};
                    if (::fstatat(::dirfd(directory.get()), native->d_name, &value, AT_SYMLINK_NOFOLLOW) != 0)
                        throw Error(ExitCode::transfer, "cannot inspect destination entry");
                    ManifestEntry entry;
                    entry.path = relative;
                    entry.size = S_ISREG(value.st_mode) ? static_cast<std::uint64_t>(value.st_size) : 0;
                    entry.modified_seconds = S_ISREG(value.st_mode) ?
                        static_cast<std::int64_t>(value.st_mtim.tv_sec) : 0;
                    if (S_ISREG(value.st_mode)) entry.type = EntryType::file;
                    else if (S_ISDIR(value.st_mode)) entry.type = EntryType::directory;
                    else entry.type = EntryType::other;
                    const auto logical = logical_times_.find(relative);
                    if (logical != logical_times_.end() && logical->second.type == entry.type &&
                        (entry.type != EntryType::file || logical->second.size == entry.size))
                        entry.modified_seconds = logical->second.modified_seconds;
                    if (entry.type == EntryType::directory) {
                        std::lock_guard lock(mutex);
                        directories.push_back({task.physical / fs::path(name), relative, task.depth + 1});
                        ++outstanding_directories;
                        ready.notify_all();
                    }
                    entries.push_back(std::move(entry));
                    if (entries.size() == worker_batch_entries && !publish(entries)) return;
                }
                if (!publish(entries)) return;
                {
                    std::lock_guard lock(mutex);
                    if (--outstanding_directories == 0) {
                        finished = true;
                        stopping = true;
                    }
                    ready.notify_all();
                }
            } catch (...) {
                fail(std::current_exception());
                return;
            }
        }
    };

    const auto hardware_threads = std::thread::hardware_concurrency();
    const auto worker_count = std::clamp(hardware_threads == 0 ? 4U : hardware_threads, 1U, 4U);
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    try {
        for (unsigned index = 0; index < worker_count; ++index) workers.emplace_back(worker);
        while (true) {
            std::vector<ManifestEntry> entries;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return failure || !completed_batches.empty() || finished; });
                if (failure) break;
                if (completed_batches.empty()) {
                    if (finished) break;
                    continue;
                }
                entries = std::move(completed_batches.front());
                completed_batches.pop_front();
                ready.notify_all();
            }
            for (auto& entry : entries) emit(std::move(entry));
        }
    } catch (...) {
        fail(std::current_exception());
    }
    for (auto& thread : workers) thread.join();
    if (failure) std::rethrow_exception(failure);
    flush();
    send_frame(client, Frame{MessageType::manifest_end, request_id, {}});
}

void Server::make_directory(ByteReader& reader) {
    const auto relative = reader.string();
    (void)reader.i64(); // Retained on the wire for protocol compatibility; directory times are ignored.
    const auto path = destination_path(relative, true);
    const auto status = fs::symlink_status(path);
    if (fs::exists(status) && !fs::is_directory(status)) fs::remove_all(path);
    fs::create_directories(path);
    if (logical_times_.erase(relative) != 0) metadata_dirty_ = true;
    if (!recovery_root_.empty())
        append_recovery_entry("INSTALL", ManifestEntry{relative, EntryType::directory, 0, 0});
}

void Server::put_begin(ByteReader& reader) {
    require_root();
    if (upload_) throw Error(ExitCode::transfer, "another upload is active");
    const auto relative = reader.string();
    if (!valid_relative_path(relative)) throw Error(ExitCode::safety, "invalid upload path");
    const auto expected_size = reader.u64();
    const auto seconds = reader.i64();
    const bool direct = decode_upload_placement(reader) == UploadPlacement::direct;
    auto upload = std::make_unique<Upload>();
    upload->path = relative;
    upload->expected_size = expected_size;
    upload->modified_seconds = seconds;
    upload->direct = direct;
    if (direct) {
        upload->temporary_path = destination_path(relative, true);
        const auto status = fs::symlink_status(upload->temporary_path);
        if (fs::exists(status) && !fs::is_regular_file(status)) fs::remove_all(upload->temporary_path);
    } else {
        upload->temporary_path = root_ / kReservedDirectory / "tmp" /
                                 (std::to_string(++upload_counter_) + ".part");
    }
    upload->stream.open(upload->temporary_path, std::ios::binary | std::ios::trunc);
    if (!upload->stream)
        throw Error(ExitCode::transfer, direct ? "cannot create destination file" : "cannot create temporary upload");
    if (direct && logical_times_.erase(relative) != 0) metadata_dirty_ = true;
    upload_ = std::move(upload);
}

void Server::put_chunk(const Frame& frame) {
    if (!upload_) throw Error(ExitCode::transfer, "no upload is active");
    if (upload_->received_size + frame.payload.size() > upload_->expected_size) throw Error(ExitCode::transfer, "upload exceeds declared size");
    upload_->stream.write(reinterpret_cast<const char*>(frame.payload.data()), static_cast<std::streamsize>(frame.payload.size()));
    if (!upload_->stream) throw Error(ExitCode::transfer, "cannot write temporary upload");
    upload_->hash.update(frame.payload);
    upload_->received_size += frame.payload.size();
}

void Server::put_end(ByteReader& reader) {
    if (!upload_) throw Error(ExitCode::transfer, "no upload is active");
    const auto supplied = reader.bytes(32);
    upload_->stream.close();
    const auto actual = upload_->hash.finalize();
    if (upload_->received_size != upload_->expected_size || !std::equal(actual.begin(), actual.end(), supplied.begin())) {
        discard_upload();
        throw Error(ExitCode::transfer, "uploaded file failed size or BLAKE3 verification");
    }
    if (upload_->direct) {
        apply_mtime(upload_->temporary_path, upload_->modified_seconds);
        logical_times_[upload_->path] =
            LogicalTime{EntryType::file, upload_->received_size, upload_->modified_seconds};
        metadata_dirty_ = true;
        if (!recovery_root_.empty())
            append_recovery_entry("INSTALL", ManifestEntry{upload_->path, EntryType::file,
                upload_->received_size, upload_->modified_seconds});
        upload_.reset();
        return;
    }
    staged_[upload_->path] = StagedFile{upload_->temporary_path, upload_->received_size, upload_->modified_seconds};
    upload_.reset();
}

void Server::discard_upload() noexcept {
    if (!upload_) return;
    upload_->stream.close();
    std::error_code ignored;
    fs::remove(upload_->temporary_path, ignored);
    upload_.reset();
}

void Server::commit_file(ByteReader& reader) {
    const auto relative = reader.string();
    const auto found = staged_.find(relative);
    if (found == staged_.end()) throw Error(ExitCode::transfer, "file was not staged");
    const auto destination = destination_path(relative, true);
    const auto status = fs::symlink_status(destination);
    if (fs::exists(status) && !fs::is_regular_file(status)) fs::remove_all(destination);
    fs::rename(found->second.temporary_path, destination);
    apply_mtime(destination, found->second.modified_seconds);
    logical_times_[relative] = LogicalTime{EntryType::file, found->second.size, found->second.modified_seconds};
    metadata_dirty_ = true;
    if (!recovery_root_.empty())
        append_recovery_entry("INSTALL", ManifestEntry{relative, EntryType::file,
            found->second.size, found->second.modified_seconds});
    staged_.erase(found);
}

void Server::delete_path(ByteReader& reader) {
    require_root();
    const auto relative = reader.string();
    if (!valid_relative_path(relative) || relative == kReservedDirectory || relative.starts_with(std::string(kReservedDirectory) + "/"))
        throw Error(ExitCode::safety, "invalid destination-relative path");
    const fs::path relative_path(relative);
    fs::path current = root_;
    for (const auto& component : relative_path.parent_path()) {
        current /= component;
        std::error_code inspection_error;
        const auto status = fs::symlink_status(current, inspection_error);
        if (inspection_error && inspection_error != std::errc::no_such_file_or_directory)
            throw Error(ExitCode::safety, "cannot inspect destination parent");
        if (fs::is_symlink(status)) throw Error(ExitCode::safety, "destination parent contains a symbolic link");
        if (!fs::exists(status) || !fs::is_directory(status)) return;
    }
    const auto destination = root_ / relative_path;
    std::error_code error;
    const auto status = fs::symlink_status(destination, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw Error(ExitCode::transfer, "cannot inspect destination path");
    if (!fs::exists(status)) return;
    const auto prefix = relative + "/";
    if (recovery_root_.empty()) {
        fs::remove_all(destination, error);
        if (error) throw Error(ExitCode::transfer, "cannot permanently delete destination path");
        for (auto iterator = logical_times_.begin(); iterator != logical_times_.end();) {
            if (iterator->first == relative || iterator->first.starts_with(prefix)) iterator = logical_times_.erase(iterator);
            else ++iterator;
        }
        metadata_dirty_ = true;
        return;
    }
    ManifestEntry quarantined{relative, EntryType::other, 0, 0};
    if (fs::is_regular_file(status)) {
        quarantined.type = EntryType::file;
        quarantined.size = fs::file_size(destination, error);
        if (error) throw Error(ExitCode::transfer, "cannot inspect destination file before quarantine");
        const auto logical = logical_times_.find(relative);
        if (logical != logical_times_.end() && logical->second.type == EntryType::file &&
            logical->second.size == quarantined.size) {
            quarantined.modified_seconds = logical->second.modified_seconds;
        } else {
            struct stat value{};
            if (::lstat(destination.c_str(), &value) != 0)
                throw Error(ExitCode::transfer, "cannot inspect destination file before quarantine");
            quarantined.modified_seconds = static_cast<std::int64_t>(value.st_mtim.tv_sec);
        }
    } else if (fs::is_directory(status)) {
        quarantined.type = EntryType::directory;
    }
    const auto recovery = recovery_root_ / relative_path;
    fs::create_directories(recovery.parent_path(), error);
    if (error) throw Error(ExitCode::transfer, "cannot create recovery directory");
    if (fs::exists(recovery)) throw Error(ExitCode::transfer, "recovery path already exists");
    fs::rename(destination, recovery, error);
    if (error) throw Error(ExitCode::transfer, "cannot quarantine destination path");
    for (const auto& [path, logical] : logical_times_) {
        if (path == relative || path.starts_with(prefix))
            append_recovery_entry("LOGICAL", ManifestEntry{path, logical.type,
                logical.size, logical.modified_seconds});
    }
    append_recovery_entry("QUARANTINE", quarantined);
    for (auto iterator = logical_times_.begin(); iterator != logical_times_.end();) {
        if (iterator->first == relative || iterator->first.starts_with(prefix)) iterator = logical_times_.erase(iterator);
        else ++iterator;
    }
    metadata_dirty_ = true;
}

void Server::set_mtime(ByteReader& reader) {
    const auto relative = reader.string();
    const auto destination = destination_path(relative, false);
    const auto seconds = reader.i64();
    const auto status = fs::symlink_status(destination);
    if (!fs::exists(status) || fs::is_symlink(status)) throw Error(ExitCode::safety, "cannot set time on missing or linked destination");
    const auto type = fs::is_directory(status) ? EntryType::directory : EntryType::file;
    if (type == EntryType::directory) {
        if (logical_times_.erase(relative) != 0) metadata_dirty_ = true;
        return;
    }
    apply_mtime(destination, seconds);
    const auto size = type == EntryType::file ? fs::file_size(destination) : 0;
    logical_times_[relative] = LogicalTime{type, size, seconds};
    metadata_dirty_ = true;
}

} // namespace syncer::server
