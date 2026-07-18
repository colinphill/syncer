#include "adb.hpp"
#include "client.hpp"
#include "process_windows.hpp"
#include "scanner.hpp"

#include "syncer/error.hpp"
#include "syncer/manifest.hpp"
#include "syncer/version.hpp"

#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>

namespace syncer::client {
namespace po = boost::program_options;

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
    std::int64_t mtime_tolerance_seconds{60};
};

void usage() {
    std::cout << "syncer " << kVersion << "\n\n"
              << "Usage:\n"
              << "  syncer init [--adb PATH] [--serial SERIAL] [--adopt] DEVICE_PATH\n"
              << "  syncer push [--adb PATH] [--serial SERIAL] [--exclude GLOB]... [--mtime-tolerance SECONDS] [--no-delete] [--direct] [--dry-run] [--json] LOCAL_SOURCE DEVICE_PATH\n"
              << "  syncer version\n";
}

Options parse(int argc, wchar_t** argv) {
    if (argc < 2) throw Error(ExitCode::usage, "missing command; use init, push, or version");
    Options options;
    options.command = narrow(argv[1]);
    std::vector<std::string> arguments;
    for (int i = 2; i < argc; ++i) arguments.push_back(narrow(argv[i]));
    if (options.command == "version") {
        if (!arguments.empty()) throw Error(ExitCode::usage, "version takes no arguments");
        return options;
    }
    if (options.command != "init" && options.command != "push") throw Error(ExitCode::usage, "unknown command: " + options.command);

    std::string adb_path;
    std::string serial;
    po::options_description description("Options");
    description.add_options()
        ("help,h", po::bool_switch(&options.help), "show help")
        ("adb", po::value<std::string>(&adb_path), "path to adb.exe")
        ("serial", po::value<std::string>(&serial), "ADB device serial");
    po::positional_options_description positional;
    if (options.command == "init") {
        description.add_options()
            ("adopt", po::bool_switch(&options.adopt), "adopt a non-empty destination")
            ("device-path", po::value<std::vector<std::string>>(&options.positional), "Android destination path");
        positional.add("device-path", 1);
    } else {
        description.add_options()
            ("exclude", po::value<std::vector<std::string>>(&options.excludes)->composing(), "case-sensitive exclusion glob")
            ("mtime-tolerance", po::value<std::int64_t>(&options.mtime_tolerance_seconds)->default_value(60),
             "ignore modification-time differences up to this many seconds")
            ("no-delete", po::bool_switch(&options.no_delete), "keep destination-only paths")
            ("direct", po::bool_switch(&options.direct), "write files without temporary staging")
            ("dry-run", po::bool_switch(&options.dry_run), "calculate without mutating")
            ("json", po::bool_switch(&options.json), "write JSON result to stdout")
            ("path", po::value<std::vector<std::string>>(&options.positional), "source and destination paths");
        positional.add("path", 2);
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
    if (!adb_path.empty()) options.adb = std::filesystem::path(widen(adb_path));
    if (!serial.empty()) options.serial = serial;
    return options;
}

std::string random_token() {
    boost::uuids::random_generator generate;
    std::string result = boost::uuids::to_string(generate()) + boost::uuids::to_string(generate());
    std::erase(result, '-');
    return result;
}

template <typename Action>
void with_server(Adb& adb, Action action) {
    const auto remote = adb.deploy_server();
    const auto token = random_token();
    const auto socket_name = std::string("syncer.") + token.substr(0, 24);
    const auto port = adb.create_forward(socket_name);
    ChildProcess server;
    try {
        server = adb.launch_server(remote, socket_name, token);
        ClientSession session(port, token);
        action(session);
        session.shutdown();
        server.finish();
        adb.remove_forward(port);
    } catch (const Error& error) {
        server.finish();
        adb.remove_forward(port);
        if (!server.output().empty())
            throw Error(error.code(), std::string(error.what()) + "; Android server: " + server.output());
        throw;
    } catch (const std::exception& error) {
        server.finish();
        adb.remove_forward(port);
        if (!server.output().empty())
            throw Error(ExitCode::transfer, std::string(error.what()) + "; Android server: " + server.output());
        throw;
    }
}

boost::json::object counts(std::size_t files, std::size_t directories, std::size_t deletions,
                           std::uint64_t bytes = 0) {
    return {{"files", files}, {"directories", directories}, {"deletions", deletions}, {"bytes", bytes}};
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
    }
    return "CHANGE";
}

void print_changes(const SyncPlan& plan) {
    if (plan.directories.empty() && plan.files.empty() && plan.deletions.empty()) {
        std::cerr << "Selected changes: none\n";
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
}

int execute(const Options& options) {
    if (options.help) { usage(); return 0; }
    if (options.command == "version") {
        std::cout << "syncer " << kVersion << " (protocol " << kProtocolVersion << ")\n";
        return 0;
    }
    if (options.command == "init") {
        if (options.positional.size() != 1) throw Error(ExitCode::usage, "init requires DEVICE_PATH");
        Adb adb(options.adb, options.serial);
        with_server(adb, [&](ClientSession& session) { session.initialize(options.positional[0], options.adopt); });
        std::cout << "Initialized " << options.positional[0] << " on " << adb.serial() << "\n";
        return 0;
    }

    if (options.positional.size() != 2) throw Error(ExitCode::usage, "push requires LOCAL_SOURCE and DEVICE_PATH");
    const auto started = std::chrono::steady_clock::now();
    const GlobSet exclusions(options.excludes);
    const auto source_path = std::filesystem::path(widen(options.positional[0]));
    std::cerr << "Scanning source...\n";
    auto source_scan = std::async(std::launch::async, [&] { return scan_source(source_path, exclusions); });
    Adb adb(options.adb, options.serial);
    SyncPlan plan;
    std::size_t applied_directories{}, applied_files{}, applied_deletions{};
    std::uint64_t transferred_bytes{};
    with_server(adb, [&](ClientSession& session) {
        std::cerr << "Scanning destination...\n";
        const auto destination = session.scan(options.positional[1]);
        const auto source = source_scan.get();
        plan = make_plan(source.manifest, destination, exclusions, !options.no_delete,
                         static_cast<std::uint64_t>(options.mtime_tolerance_seconds));
        print_changes(plan);
        if (options.dry_run) return;
        session.prepare();
        if (plan.files.empty() && plan.directories.empty() && plan.deletions.empty()) return;
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
        for (const auto& change : plan.directories) { session.make_directory(change.entry); ++applied_directories; }
        for (const auto& change : plan.files) {
            if (options.direct) {
                std::cerr << "Transferring " << change.entry.path << "\n";
                session.upload(source.files.at(change.entry.path), true);
                transferred_bytes += change.entry.size;
            }
            if (!options.direct) session.commit_file(change.entry.path);
            ++applied_files;
        }
        if (options.direct) verify_source_tree();
        for (const auto& change : plan.deletions) { session.delete_path(change.entry.path); ++applied_deletions; }
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (options.json) {
        const auto status = options.dry_run ? "dry_run" :
            (plan.files.empty() && plan.directories.empty() && plan.deletions.empty() ? "no_changes" : "success");
        boost::json::object result{
            {"schema", 1}, {"status", status}, {"device", adb.serial()},
            {"source", options.positional[0]}, {"destination", options.positional[1]},
            {"direct", options.direct},
            {"mtime_tolerance_seconds", options.mtime_tolerance_seconds},
            {"planned", counts(plan.files.size(), plan.directories.size(), plan.deletions.size(), plan.transfer_bytes)},
            {"applied", counts(applied_files, applied_directories, applied_deletions, transferred_bytes)},
            {"transferred_bytes", transferred_bytes},
            {"duration_ms", elapsed}, {"error", nullptr}};
        std::cout << boost::json::serialize(result) << '\n';
    } else {
        std::cout << (options.dry_run ? "Dry run: " : "Synchronized: ") << plan.files.size() << " file(s), "
                  << plan.directories.size() << " directorie(s), " << plan.deletions.size() << " deletion(s), "
                  << plan.transfer_bytes << " byte(s)\n";
    }
    return 0;
}

std::string error_json(int code, std::string_view message) {
    boost::json::object result{{"schema", 1}, {"status", "error"},
                               {"device", nullptr}, {"source", nullptr}, {"destination", nullptr},
                               {"planned", counts(0, 0, 0)}, {"applied", counts(0, 0, 0)},
                               {"transferred_bytes", 0}, {"duration_ms", 0},
                               {"error", boost::json::object{{"code", code}, {"message", message}}}};
    return boost::json::serialize(result);
}

} // namespace syncer::client

int wmain(int argc, wchar_t** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) if (std::wstring_view(argv[i]) == L"--json") json = true;
    try {
        if (argc == 2 && (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h")) {
            syncer::client::usage(); return 0;
        }
        return syncer::client::execute(syncer::client::parse(argc, argv));
    } catch (const syncer::Error& error) {
        if (json) std::cout << syncer::client::error_json(static_cast<int>(error.code()), error.what()) << '\n';
        else std::cerr << "syncer: " << error.what() << '\n';
        return static_cast<int>(error.code());
    } catch (const std::exception& error) {
        if (json) std::cout << syncer::client::error_json(static_cast<int>(syncer::ExitCode::transfer), error.what()) << '\n';
        else std::cerr << "syncer: " << error.what() << '\n';
        return static_cast<int>(syncer::ExitCode::transfer);
    }
}
