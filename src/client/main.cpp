#include "adb.hpp"
#include "client.hpp"
#include "process.hpp"
#include "scanner.hpp"

#include "syncer/blake3.hpp"
#include "syncer/error.hpp"
#include "syncer/manifest.hpp"
#include "syncer/version.hpp"

#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace syncer::client {
namespace po = boost::program_options;
namespace fs = std::filesystem;

struct Options {
    std::string command;
    std::optional<std::filesystem::path> adb;
    std::optional<std::string> serial;
    std::vector<std::string> excludes;
    std::vector<std::string> positional;
    bool adopt{};
    bool no_delete{};
    bool direct{};
    bool dry_run{};
    bool json{};
    bool help{};
    bool cancel_stdin{};
    std::int64_t mtime_tolerance_seconds{60};
    std::uint64_t max_removals{std::numeric_limits<std::uint64_t>::max()};
    std::optional<std::string> expected_plan;
    std::optional<std::filesystem::path> save_plan;
    std::optional<std::filesystem::path> plan_file;
    std::optional<std::string> recovery_id;
};

void usage() {
    std::cout << "syncer " << kVersion << "\n\n"
              << "Usage:\n"
              << "  syncer devices [--adb PATH] [--json]\n"
              << "  syncer init [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--adopt] DEVICE_PATH\n"
              << "  syncer push [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--exclude GLOB]... [--mtime-tolerance SECONDS] [--max-removals COUNT] [--expect-plan DIGEST] [--save-plan FILE] [--no-delete] [--direct] [--dry-run] [--json] LOCAL_SOURCE DEVICE_PATH\n"
              << "  syncer apply [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--json] --plan FILE\n"
              << "  syncer restore [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--json] --recovery ID DEVICE_PATH\n"
              << "  syncer version\n";
}

Options parse(const std::vector<std::string>& command_line) {
    if (command_line.size() < 2) throw Error(ExitCode::usage, "missing command; use devices, init, push, apply, restore, or version");
    Options options;
    options.command = command_line[1];
    std::vector<std::string> arguments(command_line.begin() + 2, command_line.end());
    if (options.command == "version") {
        if (!arguments.empty()) throw Error(ExitCode::usage, "version takes no arguments");
        return options;
    }
    if (options.command != "devices" && options.command != "init" && options.command != "push" && options.command != "apply" &&
        options.command != "restore")
        throw Error(ExitCode::usage, "unknown command: " + options.command);

    std::string adb_path;
    std::string serial;
    std::string expected_plan;
    std::string save_plan;
    std::string plan_file;
    std::string recovery_id;
    po::options_description description("Options");
    description.add_options()
        ("help,h", po::bool_switch(&options.help), "show help")
        ("adb", po::value<std::string>(&adb_path), "path to adb");
    if (options.command != "devices") {
        description.add_options()
            ("serial", po::value<std::string>(&serial), "ADB device serial")
            ("cancel-stdin", po::bool_switch(&options.cancel_stdin),
             "accept a line containing 'cancel' on stdin");
    }
    po::positional_options_description positional;
    if (options.command == "devices") {
        description.add_options()
            ("json", po::bool_switch(&options.json), "write JSON result to stdout");
    } else if (options.command == "init") {
        description.add_options()
            ("adopt", po::bool_switch(&options.adopt), "adopt a non-empty destination")
            ("device-path", po::value<std::vector<std::string>>(&options.positional), "Android destination path");
        positional.add("device-path", 1);
    } else if (options.command == "push") {
        description.add_options()
            ("exclude", po::value<std::vector<std::string>>(&options.excludes)->composing(), "case-sensitive exclusion glob")
            ("mtime-tolerance", po::value<std::int64_t>(&options.mtime_tolerance_seconds)->default_value(60),
             "ignore modification-time differences up to this many seconds")
            ("max-removals", po::value<std::uint64_t>(&options.max_removals),
             "maximum destination-only or type-conflicting entries")
            ("expect-plan", po::value<std::string>(&expected_plan),
             "require the recomputed plan to match a reviewed digest")
            ("save-plan", po::value<std::string>(&save_plan),
             "save the dry-run plan for later execution")
            ("no-delete", po::bool_switch(&options.no_delete), "keep destination-only paths")
            ("direct", po::bool_switch(&options.direct), "write files without temporary staging")
            ("dry-run", po::bool_switch(&options.dry_run), "calculate without mutating")
            ("json", po::bool_switch(&options.json), "write JSON result to stdout")
            ("path", po::value<std::vector<std::string>>(&options.positional), "source and destination paths");
        positional.add("path", 2);
    } else if (options.command == "apply") {
        description.add_options()
            ("json", po::bool_switch(&options.json), "write JSON result to stdout")
            ("plan", po::value<std::string>(&plan_file), "saved plan file to execute");
    } else {
        description.add_options()
            ("json", po::bool_switch(&options.json), "write JSON result to stdout")
            ("recovery", po::value<std::string>(&recovery_id), "recovery run identifier")
            ("device-path", po::value<std::vector<std::string>>(&options.positional), "Android destination path");
        positional.add("device-path", 1);
    }
    try {
        po::variables_map values;
        po::store(po::command_line_parser(arguments).options(description).positional(positional).run(), values);
        po::notify(values);
    } catch (const po::error& error) {
        throw Error(ExitCode::usage, error.what());
    }
    if (options.mtime_tolerance_seconds < 0)
        throw Error(ExitCode::usage, "--mtime-tolerance cannot be negative");
    if (!adb_path.empty()) options.adb = path_from_utf8(adb_path);
    if (!serial.empty()) options.serial = serial;
    if (!expected_plan.empty()) options.expected_plan = expected_plan;
    if (!save_plan.empty()) options.save_plan = path_from_utf8(save_plan);
    if (!plan_file.empty()) options.plan_file = path_from_utf8(plan_file);
    if (!recovery_id.empty()) options.recovery_id = recovery_id;
    if (options.save_plan && !options.dry_run)
        throw Error(ExitCode::usage, "--save-plan requires --dry-run");
    return options;
}

std::string random_token() {
    boost::uuids::random_generator generate;
    std::string result = boost::uuids::to_string(generate()) + boost::uuids::to_string(generate());
    std::erase(result, '-');
    return result;
}

class CancellationState {
public:
    explicit CancellationState(bool enabled) : shared_(std::make_shared<Shared>()) {
        if (enabled)
            monitor_ = std::thread([shared = shared_] {
                monitor_stdin(shared);
            });
    }

    ~CancellationState() {
        shared_->stop_requested.store(true);
        if (monitor_.joinable()) monitor_.join();
    }

    void attach(ClientSession& session) {
        std::scoped_lock lock(shared_->mutex);
        shared_->session = &session;
        if (shared_->cancelled.load()) session.cancel();
    }

    void detach(ClientSession& session) {
        std::scoped_lock lock(shared_->mutex);
        if (shared_->session == &session) shared_->session = nullptr;
    }

    [[nodiscard]] bool cancelled() const noexcept { return shared_->cancelled.load(); }

    void throw_if_cancelled() const {
        if (cancelled()) throw Error(ExitCode::cancelled, "synchronization cancelled");
    }

private:
    struct Shared {
        std::atomic<bool> cancelled{};
        std::atomic<bool> stop_requested{};
        std::mutex mutex;
        ClientSession* session{};
    };

    static void request_cancel(const std::shared_ptr<Shared>& shared) {
        shared->cancelled.store(true);
        std::scoped_lock lock(shared->mutex);
        if (shared->session) shared->session->cancel();
    }

    static bool consume(std::string& pending, const char* bytes, std::size_t count,
                        const std::shared_ptr<Shared>& shared) {
        pending.append(bytes, count);
        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "cancel") {
                request_cancel(shared);
                return true;
            }
        }
        return false;
    }

    static void monitor_stdin(const std::shared_ptr<Shared>& shared) {
        std::string pending;
        std::array<char, 128> buffer{};
#ifdef _WIN32
        const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE || ::GetFileType(input) != FILE_TYPE_PIPE)
            return;
        while (!shared->stop_requested.load()) {
            DWORD available{};
            if (!::PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) return;
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            DWORD read{};
            const auto wanted = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
            if (!::ReadFile(input, buffer.data(), wanted, &read, nullptr) || read == 0) return;
            if (consume(pending, buffer.data(), read, shared)) return;
        }
#else
        while (!shared->stop_requested.load()) {
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 50);
            if (ready < 0) return;
            if (ready == 0) continue;
            if ((descriptor.revents & POLLIN) != 0) {
                const auto read = ::read(STDIN_FILENO, buffer.data(), buffer.size());
                if (read <= 0) return;
                if (consume(pending, buffer.data(), static_cast<std::size_t>(read), shared)) return;
            } else if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return;
            }
        }
#endif
    }

    std::shared_ptr<Shared> shared_;
    std::thread monitor_;
};

template <typename Action>
void with_server(Adb& adb, CancellationState& cancellation, Action action) {
    cancellation.throw_if_cancelled();
    const auto remote = adb.deploy_server();
    cancellation.throw_if_cancelled();
    const auto token = random_token();
    const auto socket_name = std::string("syncer.") + token.substr(0, 24);
    const auto port = adb.create_forward(socket_name);
    ChildProcess server;
    try {
        server = adb.launch_server(remote, socket_name, token);
        ClientSession session(port, token);
        cancellation.attach(session);
        try {
            cancellation.throw_if_cancelled();
            action(session);
            cancellation.throw_if_cancelled();
            session.shutdown();
            cancellation.detach(session);
            cancellation.throw_if_cancelled();
        } catch (...) {
            cancellation.detach(session);
            throw;
        }
        server.finish();
        adb.remove_forward(port);
    } catch (const Error& error) {
        server.finish();
        adb.remove_forward(port);
        if (cancellation.cancelled())
            throw Error(ExitCode::cancelled, "synchronization cancelled");
        if (!server.output().empty())
            throw Error(error.code(), std::string(error.what()) + "; Android server: " + server.output());
        throw;
    } catch (const std::exception& error) {
        server.finish();
        adb.remove_forward(port);
        if (cancellation.cancelled())
            throw Error(ExitCode::cancelled, "synchronization cancelled");
        if (!server.output().empty())
            throw Error(ExitCode::transfer, std::string(error.what()) + "; Android server: " + server.output());
        throw;
    }
}

boost::json::object counts(std::size_t files, std::size_t directories, std::size_t deletions,
                           std::uint64_t bytes = 0, std::size_t timestamps = 0) {
    return {{"files", files}, {"directories", directories}, {"deletions", deletions}, {"bytes", bytes},
            {"timestamps", timestamps}};
}

bool has_content_changes(const SyncPlan& plan) {
    return !plan.files.empty() || !plan.directories.empty() || !plan.deletions.empty();
}

std::string_view action_name(PlanAction action) {
    switch (action) {
    case PlanAction::create_directory: return "CREATE DIRECTORY";
    case PlanAction::replace_with_directory: return "REPLACE WITH DIRECTORY";
    case PlanAction::add_file: return "ADD FILE";
    case PlanAction::update_file: return "UPDATE FILE";
    case PlanAction::replace_with_file: return "REPLACE WITH FILE";
    case PlanAction::delete_file: return "DELETE FILE";
    case PlanAction::delete_directory: return "DELETE DIRECTORY";
    case PlanAction::delete_other: return "DELETE ENTRY";
    case PlanAction::record_timestamp: return "RECORD TIMESTAMP";
    }
    return "CHANGE";
}

std::string_view action_id(PlanAction action) {
    switch (action) {
    case PlanAction::create_directory: return "create_directory";
    case PlanAction::replace_with_directory: return "replace_with_directory";
    case PlanAction::add_file: return "add_file";
    case PlanAction::update_file: return "update_file";
    case PlanAction::replace_with_file: return "replace_with_file";
    case PlanAction::delete_file: return "delete_file";
    case PlanAction::delete_directory: return "delete_directory";
    case PlanAction::delete_other: return "delete_other";
    case PlanAction::record_timestamp: return "record_timestamp";
    }
    return "unknown";
}

PlanAction parse_action_id(std::string_view value) {
    if (value == "create_directory") return PlanAction::create_directory;
    if (value == "replace_with_directory") return PlanAction::replace_with_directory;
    if (value == "add_file") return PlanAction::add_file;
    if (value == "update_file") return PlanAction::update_file;
    if (value == "replace_with_file") return PlanAction::replace_with_file;
    if (value == "delete_file") return PlanAction::delete_file;
    if (value == "delete_directory") return PlanAction::delete_directory;
    if (value == "delete_other") return PlanAction::delete_other;
    if (value == "record_timestamp") return PlanAction::record_timestamp;
    throw Error(ExitCode::safety, "saved plan contains an unknown action");
}

void append_change(ByteWriter& writer, const PlannedChange& change) {
    writer.u8(static_cast<std::uint8_t>(change.action));
    encode_manifest_entry(writer, change.entry);
    writer.string(change.reason);
}

std::string plan_digest(const SyncPlan& plan, std::string_view source, std::string_view destination,
                        std::string_view serial, const Options& options) {
    ByteWriter writer;
    writer.string("SYNCER-PLAN"); writer.u32(2);
    writer.string(source); writer.string(destination); writer.string(serial);
    writer.u8(options.no_delete ? 1 : 0);
    writer.u8(options.direct ? 1 : 0);
    writer.i64(options.mtime_tolerance_seconds);
    writer.u32(static_cast<std::uint32_t>(options.excludes.size()));
    for (const auto& exclude : options.excludes) writer.string(exclude);
    writer.u32(static_cast<std::uint32_t>(plan.directories.size()));
    for (const auto& change : plan.directories) append_change(writer, change);
    writer.u32(static_cast<std::uint32_t>(plan.files.size()));
    for (const auto& change : plan.files) append_change(writer, change);
    writer.u32(static_cast<std::uint32_t>(plan.deletions.size()));
    for (const auto& change : plan.deletions) append_change(writer, change);
    writer.u32(static_cast<std::uint32_t>(plan.timestamps.size()));
    for (const auto& change : plan.timestamps) append_change(writer, change);
    const auto bytes = std::move(writer).take();
    Blake3 hash;
    hash.update(std::span<const std::uint8_t>(bytes));
    return digest_hex(hash.finalize());
}

struct SavedPlan {
    std::string source;
    std::string destination;
    std::string serial;
    Options options;
    SyncPlan plan;
    std::string digest;
    std::uint64_t removals{};
    std::unordered_map<std::string, SourceFile> source_files;
    std::vector<DestinationPrecondition> preconditions;
};

boost::json::object entry_json(const ManifestEntry& entry) {
    return {{"path", entry.path}, {"type", static_cast<std::uint64_t>(entry.type)},
            {"size", entry.size}, {"modified_seconds", entry.modified_seconds}};
}

ManifestEntry parse_entry(const boost::json::value& value) {
    const auto& object = value.as_object();
    ManifestEntry entry;
    entry.path = boost::json::value_to<std::string>(object.at("path"));
    const auto type = object.at("type").to_number<std::uint64_t>();
    if (type < static_cast<std::uint64_t>(EntryType::file) ||
        type > static_cast<std::uint64_t>(EntryType::other) || !valid_relative_path(entry.path))
        throw Error(ExitCode::safety, "saved plan contains an invalid entry");
    entry.type = static_cast<EntryType>(type);
    entry.size = object.at("size").to_number<std::uint64_t>();
    entry.modified_seconds = object.at("modified_seconds").to_number<std::int64_t>();
    return entry;
}

boost::json::array changes_json(const std::vector<PlannedChange>& changes,
                                const SourceTree* source = nullptr) {
    boost::json::array result;
    result.reserve(changes.size());
    for (const auto& change : changes) {
        boost::json::object object{{"action", action_id(change.action)},
                                   {"entry", entry_json(change.entry)},
                                   {"reason", change.reason}};
        if (source) object["source_path"] = path_to_utf8(source->files.at(change.entry.path).physical_path);
        result.emplace_back(std::move(object));
    }
    return result;
}

std::vector<DestinationPrecondition> build_preconditions(const SyncPlan& plan,
                                                          const Manifest& destination) {
    std::map<std::string, DestinationPrecondition, std::less<>> values;
    const auto add = [&](std::string_view path) {
        if (path.empty() || values.contains(path)) return;
        const auto found = destination.find(std::string(path));
        if (found == destination.end())
            values.emplace(path, DestinationPrecondition{std::string(path), false, {}});
        else
            values.emplace(path, DestinationPrecondition{found->first, true, found->second});
    };
    const auto add_with_parents = [&](std::string_view path) {
        add(path);
        auto parent = path;
        while (true) {
            const auto separator = parent.rfind('/');
            if (separator == std::string_view::npos) break;
            parent = parent.substr(0, separator);
            add(parent);
        }
    };
    const auto add_change = [&](const PlannedChange& change) {
        add_with_parents(change.entry.path);
        if (change.action != PlanAction::replace_with_directory &&
            change.action != PlanAction::replace_with_file) return;
        const auto prefix = change.entry.path + "/";
        for (const auto& [path, entry] : destination) {
            (void)entry;
            if (path.starts_with(prefix)) add(path);
        }
    };
    for (const auto& change : plan.directories) add_change(change);
    for (const auto& change : plan.files) add_change(change);
    for (const auto& change : plan.deletions) add_change(change);
    for (const auto& change : plan.timestamps) add_change(change);
    std::vector<DestinationPrecondition> result;
    result.reserve(values.size());
    for (auto& [path, value] : values) result.push_back(std::move(value));
    return result;
}

void save_plan_file(const fs::path& path, const SyncPlan& plan, const SourceTree& source,
                    const Manifest& destination, const Options& options, std::string_view serial,
                    std::string_view digest, std::uint64_t removals) {
    boost::json::array excludes;
    for (const auto& exclude : options.excludes) excludes.emplace_back(exclude);
    boost::json::array preconditions;
    for (const auto& precondition : build_preconditions(plan, destination)) {
        boost::json::object object{{"path", precondition.path}, {"exists", precondition.exists}};
        if (precondition.exists) object["entry"] = entry_json(precondition.entry);
        preconditions.emplace_back(std::move(object));
    }
    boost::json::object root{
        {"schema", 2}, {"syncer_version", kVersion}, {"protocol", kProtocolVersion},
        {"source", options.positional[0]}, {"destination", options.positional[1]},
        {"device", serial}, {"direct", options.direct}, {"no_delete", options.no_delete},
        {"mtime_tolerance_seconds", options.mtime_tolerance_seconds},
        {"excludes", std::move(excludes)}, {"plan_digest", digest}, {"removal_count", removals},
        {"directories", changes_json(plan.directories)}, {"files", changes_json(plan.files, &source)},
        {"deletions", changes_json(plan.deletions)}, {"timestamps", changes_json(plan.timestamps)},
        {"preconditions", std::move(preconditions)}};
    const auto serialized = boost::json::serialize(root);
    std::error_code error;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), error);
    if (error) throw Error(ExitCode::transfer, "cannot create saved-plan directory");
    const auto temporary = path.string() + ".tmp." + random_token().substr(0, 12);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        if (!output) throw Error(ExitCode::transfer, "cannot write saved plan");
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary);
        throw Error(ExitCode::transfer, "cannot commit saved plan; the output file may already exist");
    }
}

std::vector<PlannedChange> parse_changes(const boost::json::array& array, SavedPlan& saved,
                                         bool source_files) {
    if (array.size() > 1000000U) throw Error(ExitCode::safety, "saved plan has too many actions");
    std::vector<PlannedChange> result;
    result.reserve(array.size());
    for (const auto& value : array) {
        const auto& object = value.as_object();
        PlannedChange change{parse_entry(object.at("entry")),
            parse_action_id(boost::json::value_to<std::string>(object.at("action"))),
            boost::json::value_to<std::string>(object.at("reason"))};
        if (source_files) {
            const auto physical = path_from_utf8(
                boost::json::value_to<std::string>(object.at("source_path")));
            if (!saved.source_files.emplace(change.entry.path, SourceFile{change.entry, physical}).second)
                throw Error(ExitCode::safety, "saved plan contains duplicate source files");
        }
        result.push_back(std::move(change));
    }
    return result;
}

SavedPlan load_plan_file(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    constexpr std::uint64_t maximum_size = 256ULL * 1024ULL * 1024ULL;
    if (error || size > maximum_size) throw Error(ExitCode::safety, "saved plan is missing or too large");
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input && !text.empty()) throw Error(ExitCode::transfer, "cannot read saved plan");
    try {
        const auto document = boost::json::parse(text);
        const auto& root = document.as_object();
        if (root.at("schema").to_number<std::uint64_t>() != 2 ||
            root.at("protocol").to_number<std::uint64_t>() != kProtocolVersion)
            throw Error(ExitCode::safety, "saved plan has an unsupported schema or protocol");
        SavedPlan saved;
        saved.source = boost::json::value_to<std::string>(root.at("source"));
        saved.destination = boost::json::value_to<std::string>(root.at("destination"));
        saved.serial = boost::json::value_to<std::string>(root.at("device"));
        saved.digest = boost::json::value_to<std::string>(root.at("plan_digest"));
        saved.removals = root.at("removal_count").to_number<std::uint64_t>();
        saved.options.no_delete = root.at("no_delete").as_bool();
        saved.options.direct = root.at("direct").as_bool();
        saved.options.mtime_tolerance_seconds =
            root.at("mtime_tolerance_seconds").to_number<std::int64_t>();
        for (const auto& exclude : root.at("excludes").as_array())
            saved.options.excludes.push_back(boost::json::value_to<std::string>(exclude));
        saved.options.positional = {saved.source, saved.destination};
        saved.plan.directories = parse_changes(root.at("directories").as_array(), saved, false);
        saved.plan.files = parse_changes(root.at("files").as_array(), saved, true);
        saved.plan.deletions = parse_changes(root.at("deletions").as_array(), saved, false);
        saved.plan.timestamps = parse_changes(root.at("timestamps").as_array(), saved, false);
        for (const auto& change : saved.plan.directories)
            if (change.entry.type != EntryType::directory ||
                (change.action != PlanAction::create_directory &&
                 change.action != PlanAction::replace_with_directory))
                throw Error(ExitCode::safety, "saved plan contains an invalid directory action");
        for (const auto& change : saved.plan.files)
            if (change.entry.type != EntryType::file ||
                (change.action != PlanAction::add_file && change.action != PlanAction::update_file &&
                 change.action != PlanAction::replace_with_file))
                throw Error(ExitCode::safety, "saved plan contains an invalid file action");
        for (const auto& change : saved.plan.deletions)
            if ((change.entry.type == EntryType::file && change.action != PlanAction::delete_file) ||
                (change.entry.type == EntryType::directory && change.action != PlanAction::delete_directory) ||
                (change.entry.type == EntryType::other && change.action != PlanAction::delete_other))
                throw Error(ExitCode::safety, "saved plan contains an invalid deletion action");
        for (const auto& change : saved.plan.timestamps)
            if (change.entry.type != EntryType::file || change.action != PlanAction::record_timestamp)
                throw Error(ExitCode::safety, "saved plan contains an invalid timestamp record");
        for (const auto& change : saved.plan.files) saved.plan.transfer_bytes += change.entry.size;
        for (const auto& value : root.at("preconditions").as_array()) {
            const auto& object = value.as_object();
            DestinationPrecondition precondition;
            precondition.path = boost::json::value_to<std::string>(object.at("path"));
            precondition.exists = object.at("exists").as_bool();
            if (precondition.exists) {
                precondition.entry = parse_entry(object.at("entry"));
                if (precondition.entry.path != precondition.path)
                    throw Error(ExitCode::safety, "saved plan contains an invalid precondition");
            } else if (!valid_relative_path(precondition.path)) {
                throw Error(ExitCode::safety, "saved plan contains an invalid precondition path");
            }
            saved.preconditions.push_back(std::move(precondition));
        }
        const auto actual_digest = plan_digest(saved.plan, saved.source, saved.destination,
                                               saved.serial, saved.options);
        if (actual_digest != saved.digest)
            throw Error(ExitCode::safety, "saved plan digest does not match its contents");
        return saved;
    } catch (const Error&) {
        throw;
    } catch (const std::exception&) {
        throw Error(ExitCode::safety, "saved plan is invalid");
    }
}

boost::json::array plan_actions(const SyncPlan& plan) {
    boost::json::array actions;
    const auto append = [&](const PlannedChange& change) {
        actions.emplace_back(boost::json::object{
            {"action", action_id(change.action)}, {"path", change.entry.path},
            {"reason", change.reason},
            {"type", change.entry.type == EntryType::directory ? "directory" :
                change.entry.type == EntryType::file ? "file" : "other"},
            {"size", change.entry.size}, {"modified_seconds", change.entry.modified_seconds}});
    };
    for (const auto& change : plan.directories) append(change);
    for (const auto& change : plan.files) append(change);
    for (const auto& change : plan.deletions) append(change);
    return actions;
}

std::uint64_t removal_count(const SyncPlan& plan, const Manifest& destination) {
    std::set<std::string> removals;
    for (const auto& change : plan.deletions) removals.insert(change.entry.path);
    const auto add_conflict = [&](const PlannedChange& change) {
        if (change.action != PlanAction::replace_with_directory &&
            change.action != PlanAction::replace_with_file) return;
        const auto prefix = change.entry.path + "/";
        for (const auto& [path, entry] : destination) {
            (void)entry;
            if (path == change.entry.path || path.starts_with(prefix)) removals.insert(path);
        }
    };
    for (const auto& change : plan.directories) add_conflict(change);
    for (const auto& change : plan.files) add_conflict(change);
    return removals.size();
}

void print_timestamp_records(const SyncPlan& plan) {
    if (plan.timestamps.empty()) return;
    std::cerr << "Timestamp records: " << plan.timestamps.size()
              << " unchanged file(s) will have their destination time recorded without transferring data\n";
}

void print_changes(const SyncPlan& plan) {
    if (!has_content_changes(plan)) {
        std::cerr << "Selected changes: none\n";
        print_timestamp_records(plan);
        return;
    }
    std::cerr << "Selected changes:\n";
    const auto print = [](const PlannedChange& change) {
        std::cerr << "  " << action_name(change.action) << ' ' << std::quoted(change.entry.path)
                  << " -- " << change.reason << '\n';
    };
    for (const auto& change : plan.directories) print(change);
    for (const auto& change : plan.files) print(change);
    for (const auto& change : plan.deletions) print(change);
    print_timestamp_records(plan);
}

std::size_t record_timestamps(ClientSession& session, const SyncPlan& plan) {
    if (plan.timestamps.empty()) return 0;
    std::cerr << "Recording " << plan.timestamps.size() << " destination timestamp(s)\n";
    session.record_times(plan.timestamps);
    return plan.timestamps.size();
}

void apply_deletions(ClientSession& session, const SyncPlan& plan,
                     bool recovery_enabled, std::size_t& applied_deletions,
                     std::size_t& applied_quarantines) {
    std::set<std::string, std::less<>> paths;
    for (const auto& change : plan.deletions) paths.insert(change.entry.path);
    for (const auto& change : plan.deletions) {
        std::string_view parent = change.entry.path;
        bool covered{};
        while (true) {
            const auto separator = parent.rfind('/');
            if (separator == std::string_view::npos) break;
            parent = parent.substr(0, separator);
            if (paths.contains(parent)) { covered = true; break; }
        }
        if (covered) continue;
        session.delete_path(change.entry.path);
        if (recovery_enabled) ++applied_quarantines;
    }
    applied_deletions = plan.deletions.size();
}

int execute_saved_plan(const Options& options, CancellationState& cancellation) {
    if (!options.plan_file) throw Error(ExitCode::usage, "apply requires --plan FILE");
    const auto started = std::chrono::steady_clock::now();
    SavedPlan saved = load_plan_file(*options.plan_file);
    std::optional<std::string> selected = options.serial;
    if (!selected) selected = saved.serial;
    Adb adb(options.adb, selected);
    if (adb.serial() != saved.serial)
        throw Error(ExitCode::safety, "saved plan belongs to a different Android device");

    std::string recovery_id;
    std::size_t applied_directories{}, applied_files{}, applied_deletions{}, applied_quarantines{};
    std::size_t applied_timestamps{};
    std::uint64_t transferred_bytes{};
    print_changes(saved.plan);
    with_server(adb, cancellation, [&](ClientSession& session) {
        session.select_root(saved.destination);
        session.validate_plan(saved.preconditions);
        if (!has_content_changes(saved.plan)) {
            applied_timestamps = record_timestamps(session, saved.plan);
            return;
        }
        if (!saved.options.direct) {
            recovery_id = random_token().substr(0, 24);
            session.prepare(recovery_id);
        }
        if (!saved.options.direct) {
            for (const auto& change : saved.plan.files) {
                std::cerr << "Staging " << change.entry.path << "\n";
                session.upload(saved.source_files.at(change.entry.path), false);
                transferred_bytes += change.entry.size;
            }
        }
        for (const auto& change : saved.plan.directories) {
            if (change.action == PlanAction::replace_with_directory) {
                session.delete_path(change.entry.path);
                if (!saved.options.direct) ++applied_quarantines;
            }
            session.make_directory(change.entry);
            ++applied_directories;
        }
        for (const auto& change : saved.plan.files) {
            if (change.action == PlanAction::update_file || change.action == PlanAction::replace_with_file) {
                session.delete_path(change.entry.path);
                if (!saved.options.direct) ++applied_quarantines;
            }
            if (saved.options.direct) {
                std::cerr << "Transferring " << change.entry.path << "\n";
                session.upload(saved.source_files.at(change.entry.path), true);
                transferred_bytes += change.entry.size;
            } else {
                session.commit_file(change.entry.path);
            }
            ++applied_files;
        }
        apply_deletions(session, saved.plan, !saved.options.direct,
                        applied_deletions, applied_quarantines);
        applied_timestamps = record_timestamps(session, saved.plan);
    });

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (options.json) {
        const auto status = has_content_changes(saved.plan) ? "success" : "no_changes";
        boost::json::object result{
            {"schema", 1}, {"status", status}, {"device", adb.serial()},
            {"source", saved.source}, {"destination", saved.destination},
            {"direct", saved.options.direct},
            {"mtime_tolerance_seconds", saved.options.mtime_tolerance_seconds},
            {"max_removals", nullptr}, {"removal_count", saved.removals},
            {"plan_digest", saved.digest}, {"plan_file", path_to_utf8(*options.plan_file)},
            {"actions", plan_actions(saved.plan)},
            {"recovery_id", recovery_id.empty() ? boost::json::value(nullptr) : boost::json::value(recovery_id)},
            {"planned", counts(saved.plan.files.size(), saved.plan.directories.size(),
                               saved.plan.deletions.size(), saved.plan.transfer_bytes,
                               saved.plan.timestamps.size())},
            {"applied", counts(applied_files, applied_directories, applied_deletions, transferred_bytes,
                               applied_timestamps)},
            {"quarantined_count", applied_quarantines}, {"transferred_bytes", transferred_bytes},
            {"duration_ms", elapsed}, {"error", nullptr}};
        std::cout << boost::json::serialize(result) << '\n';
    } else {
        std::cout << "Applied saved plan: " << saved.plan.files.size() << " file(s), "
                  << saved.plan.directories.size() << " directorie(s), "
                  << saved.plan.deletions.size() << " deletion(s), "
                  << saved.plan.transfer_bytes << " byte(s)";
        if (applied_timestamps != 0) std::cout << ", " << applied_timestamps << " timestamp record(s)";
        std::cout << '\n';
    }
    return 0;
}

int execute_restore(const Options& options, CancellationState& cancellation) {
    if (!options.recovery_id) throw Error(ExitCode::usage, "restore requires --recovery ID");
    if (options.positional.size() != 1) throw Error(ExitCode::usage, "restore requires DEVICE_PATH");
    const auto started = std::chrono::steady_clock::now();
    Adb adb(options.adb, options.serial);
    with_server(adb, cancellation, [&](ClientSession& session) {
        session.select_root(options.positional[0]);
        session.restore(*options.recovery_id);
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (options.json) {
        boost::json::object result{
            {"schema", 1}, {"status", "restored"}, {"device", adb.serial()},
            {"destination", options.positional[0]}, {"recovery_id", *options.recovery_id},
            {"duration_ms", elapsed}, {"error", nullptr}};
        std::cout << boost::json::serialize(result) << '\n';
    } else {
        std::cout << "Restored recovery run " << *options.recovery_id << " at "
                  << options.positional[0] << " on " << adb.serial() << '\n';
    }
    return 0;
}

int execute_devices(const Options& options) {
    const auto devices = Adb::enumerate(options.adb);
    if (options.json) {
        const auto metadata = [](const std::string& value) {
            return value.empty() ? boost::json::value(nullptr) : boost::json::value(value);
        };
        boost::json::array items;
        for (const auto& device : devices) {
            items.emplace_back(boost::json::object{
                {"id", device.id},
                {"serial", device.serial},
                {"display_name", device.display_name},
                {"state", device.state},
                {"ready", device.ready()},
                {"model", metadata(device.model)},
                {"product", metadata(device.product)},
                {"device", metadata(device.device)},
                {"transport_id", metadata(device.transport_id)},
                {"connection", device.connection}});
        }
        std::cout << boost::json::serialize(boost::json::object{
            {"schema", 1}, {"status", "success"}, {"devices", std::move(items)}, {"error", nullptr}}) << '\n';
    } else if (devices.empty()) {
        std::cout << "No Android devices found.\n";
    } else {
        std::cout << "ID\tSERIAL\tSTATE\tCONNECTION\tNAME\n";
        for (const auto& device : devices)
            std::cout << device.id << '\t' << device.serial << '\t' << device.state << '\t'
                      << device.connection << '\t' << device.display_name << '\n';
    }
    return 0;
}

int execute(const Options& options, CancellationState& cancellation) {
    if (options.help) { usage(); return 0; }
    if (options.command == "version") {
        std::cout << "syncer " << kVersion << " (protocol " << kProtocolVersion << ")\n";
        return 0;
    }
    if (options.command == "devices") return execute_devices(options);
    cancellation.throw_if_cancelled();
    if (options.command == "init") {
        if (options.positional.size() != 1) throw Error(ExitCode::usage, "init requires DEVICE_PATH");
        Adb adb(options.adb, options.serial);
        with_server(adb, cancellation,
                    [&](ClientSession& session) { session.initialize(options.positional[0], options.adopt); });
        std::cout << "Initialized " << options.positional[0] << " on " << adb.serial() << "\n";
        return 0;
    }
    if (options.command == "apply") return execute_saved_plan(options, cancellation);
    if (options.command == "restore") return execute_restore(options, cancellation);

    if (options.positional.size() != 2) throw Error(ExitCode::usage, "push requires LOCAL_SOURCE and DEVICE_PATH");
    const auto started = std::chrono::steady_clock::now();
    const GlobSet exclusions(options.excludes);
    const auto source_path = path_from_utf8(options.positional[0]);
    std::cerr << "Scanning source...\n";
    auto source_scan = std::async(std::launch::async, [&] { return scan_source(source_path, exclusions); });
    Adb adb(options.adb, options.serial);
    SyncPlan plan;
    std::string digest;
    std::string recovery_id;
    std::uint64_t planned_removals{};
    std::size_t applied_directories{}, applied_files{}, applied_deletions{}, applied_quarantines{};
    std::size_t applied_timestamps{};
    std::uint64_t transferred_bytes{};
    with_server(adb, cancellation, [&](ClientSession& session) {
        std::cerr << "Scanning destination...\n";
        const auto scanned = session.scan(options.positional[1]);
        const auto& destination = scanned.entries;
        const auto source = source_scan.get();
        plan = make_plan(source.manifest, destination, exclusions, !options.no_delete,
                         static_cast<std::uint64_t>(options.mtime_tolerance_seconds),
                         scanned.untracked_times);
        digest = plan_digest(plan, options.positional[0], options.positional[1], adb.serial(), options);
        planned_removals = removal_count(plan, destination);
        print_changes(plan);
        if (planned_removals > options.max_removals)
            throw Error(ExitCode::safety, "plan removes " + std::to_string(planned_removals) +
                " entries, exceeding --max-removals " + std::to_string(options.max_removals));
        if (options.expected_plan && *options.expected_plan != digest)
            throw Error(ExitCode::source_changed, "the synchronization plan changed after preview");
        if (options.save_plan)
            save_plan_file(*options.save_plan, plan, source, destination, options,
                           adb.serial(), digest, planned_removals);
        if (options.dry_run) return;
        if (!has_content_changes(plan)) {
            applied_timestamps = record_timestamps(session, plan);
            return;
        }
        if (!options.direct) {
            recovery_id = random_token().substr(0, 24);
            session.prepare(recovery_id);
        }
        const auto verify_source_tree = [&] {
            try {
                const auto current_source = scan_source(source_path, exclusions);
                if (current_source.manifest != source.manifest)
                    throw Error(ExitCode::source_changed, "source tree changed after it was scanned");
            } catch (const Error& error) {
                if (error.code() == ExitCode::source_changed) throw;
                throw Error(ExitCode::source_changed,
                            "source tree changed after it was scanned: " + std::string(error.what()));
            }
        };
        if (!options.direct) {
            for (const auto& change : plan.files) {
                std::cerr << "Staging " << change.entry.path << "\n";
                session.upload(source.files.at(change.entry.path), false);
                transferred_bytes += change.entry.size;
            }
            verify_source_tree();
        } else {
            // Direct mode starts mutating before all transfers complete, so do the
            // whole-tree preflight before applying its first planned change.
            verify_source_tree();
        }
        for (const auto& change : plan.directories) {
            if (change.action == PlanAction::replace_with_directory) {
                session.delete_path(change.entry.path);
                if (!options.direct) ++applied_quarantines;
            }
            session.make_directory(change.entry);
            ++applied_directories;
        }
        for (const auto& change : plan.files) {
            if (change.action == PlanAction::update_file || change.action == PlanAction::replace_with_file) {
                session.delete_path(change.entry.path);
                if (!options.direct) ++applied_quarantines;
            }
            if (options.direct) {
                std::cerr << "Transferring " << change.entry.path << "\n";
                session.upload(source.files.at(change.entry.path), true);
                transferred_bytes += change.entry.size;
            }
            if (!options.direct) session.commit_file(change.entry.path);
            ++applied_files;
        }
        if (options.direct) verify_source_tree();
        apply_deletions(session, plan, !options.direct,
                        applied_deletions, applied_quarantines);
        applied_timestamps = record_timestamps(session, plan);
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (options.json) {
        const auto status = options.dry_run ? "dry_run" :
            (has_content_changes(plan) ? "success" : "no_changes");
        boost::json::object result{
            {"schema", 1}, {"status", status}, {"device", adb.serial()},
            {"source", options.positional[0]}, {"destination", options.positional[1]},
            {"direct", options.direct},
            {"mtime_tolerance_seconds", options.mtime_tolerance_seconds},
            {"max_removals", options.max_removals == std::numeric_limits<std::uint64_t>::max()
                ? boost::json::value(nullptr) : boost::json::value(options.max_removals)},
            {"removal_count", planned_removals},
            {"plan_digest", digest},
            {"plan_file", options.save_plan ? boost::json::value(path_to_utf8(*options.save_plan)) : boost::json::value(nullptr)},
            {"actions", plan_actions(plan)},
            {"recovery_id", recovery_id.empty() ? boost::json::value(nullptr) : boost::json::value(recovery_id)},
            {"planned", counts(plan.files.size(), plan.directories.size(), plan.deletions.size(), plan.transfer_bytes,
                               plan.timestamps.size())},
            {"applied", counts(applied_files, applied_directories, applied_deletions, transferred_bytes,
                               applied_timestamps)},
            {"quarantined_count", applied_quarantines},
            {"transferred_bytes", transferred_bytes},
            {"duration_ms", elapsed}, {"error", nullptr}};
        std::cout << boost::json::serialize(result) << '\n';
    } else {
        std::cout << (options.dry_run ? "Dry run: " : "Synchronized: ") << plan.files.size() << " file(s), "
                  << plan.directories.size() << " directorie(s), " << plan.deletions.size() << " deletion(s), "
                  << plan.transfer_bytes << " byte(s)";
        const auto timestamps = options.dry_run ? plan.timestamps.size() : applied_timestamps;
        if (timestamps != 0) std::cout << ", " << timestamps << " timestamp record(s)";
        std::cout << '\n';
    }
    return 0;
}

std::string error_json(int code, std::string_view message) {
    boost::json::object result{{"schema", 1}, {"status", "error"},
                               {"device", nullptr}, {"source", nullptr}, {"destination", nullptr},
                               {"plan_digest", nullptr}, {"actions", boost::json::array{}},
                               {"plan_file", nullptr},
                               {"recovery_id", nullptr}, {"removal_count", 0},
                               {"planned", counts(0, 0, 0)}, {"applied", counts(0, 0, 0)},
                               {"quarantined_count", 0},
                               {"transferred_bytes", 0}, {"duration_ms", 0},
                               {"error", boost::json::object{{"code", code}, {"message", message}}}};
    return boost::json::serialize(result);
}

std::string devices_error_json(int code, std::string_view message) {
    return boost::json::serialize(boost::json::object{
        {"schema", 1}, {"status", "error"}, {"devices", boost::json::array{}},
        {"error", boost::json::object{{"code", code}, {"message", message}}}});
}

int run(std::vector<std::string> command_line) {
    const bool devices_command = command_line.size() > 1 && command_line[1] == "devices";
    const bool json = std::find(command_line.begin() + std::min<std::size_t>(1, command_line.size()),
                                command_line.end(), "--json") != command_line.end();
    try {
        if (command_line.size() == 2 && (command_line[1] == "--help" || command_line[1] == "-h")) {
            usage(); return 0;
        }
        const auto options = parse(command_line);
        CancellationState cancellation(options.cancel_stdin);
        return execute(options, cancellation);
    } catch (const Error& error) {
        if (json) std::cout << (devices_command
            ? devices_error_json(static_cast<int>(error.code()), error.what())
            : error_json(static_cast<int>(error.code()), error.what())) << '\n';
        else std::cerr << "syncer: " << error.what() << '\n';
        return static_cast<int>(error.code());
    } catch (const std::exception& error) {
        if (json) std::cout << (devices_command
            ? devices_error_json(static_cast<int>(ExitCode::transfer), error.what())
            : error_json(static_cast<int>(ExitCode::transfer), error.what())) << '\n';
        else std::cerr << "syncer: " << error.what() << '\n';
        return static_cast<int>(ExitCode::transfer);
    }
}

} // namespace syncer::client

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    const syncer::client::Utf8ConsoleOutput console_output;
    try {
        std::vector<std::string> command_line;
        command_line.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index)
            command_line.push_back(syncer::client::utf8_from_wide(argv[index]));
        return syncer::client::run(std::move(command_line));
    } catch (const syncer::Error& error) {
        std::cerr << "syncer: " << error.what() << '\n';
        return static_cast<int>(error.code());
    }
}
#else
int main(int argc, char** argv) {
    const syncer::client::Utf8ConsoleOutput console_output;
    std::vector<std::string> command_line;
    command_line.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) command_line.emplace_back(argv[index]);
    return syncer::client::run(std::move(command_line));
}
#endif
