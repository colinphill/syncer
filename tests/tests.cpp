#include "syncer/blake3.hpp"
#include "syncer/error.hpp"
#include "syncer/glob.hpp"
#include "syncer/manifest.hpp"
#include "syncer/protocol.hpp"

#include "scanner.hpp"
#include "process.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
int failures{};

std::map<std::string, std::string> protocol_vectors() {
    std::ifstream input(std::filesystem::path(SYNCER_TEST_SOURCE_DIR) / "tests" / "protocol-v6-vectors.txt");
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator != std::string::npos) values.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
    return values;
}

std::string bytes_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = digits[bytes[index] >> 4U];
        result[index * 2 + 1] = digits[bytes[index] & 0xFU];
    }
    return result;
}
void check(bool condition, const char* expression, int line) {
    if (!condition) { std::cerr << "line " << line << ": check failed: " << expression << '\n'; ++failures; }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <typename Action>
bool throws_syncer_error(Action action) {
    try { action(); }
    catch (const syncer::Error&) { return true; }
    return false;
}

void test_blake3() {
    syncer::Blake3 empty;
    CHECK(syncer::digest_hex(empty.finalize()) == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    const std::string abc = "abc";
    syncer::Blake3 hasher;
    hasher.update({reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()});
    CHECK(syncer::digest_hex(hasher.finalize()) == "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
    std::vector<std::uint8_t> data(4097);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 31U);
    syncer::Blake3 once; once.update(data);
    syncer::Blake3 pieces;
    for (std::size_t i = 0; i < data.size(); i += 17)
        pieces.update(std::span(data).subspan(i, std::min<std::size_t>(17, data.size() - i)));
    CHECK(once.finalize() == pieces.finalize());
    std::vector<std::uint8_t> official_input(2048);
    for (std::size_t i = 0; i < official_input.size(); ++i) official_input[i] = static_cast<std::uint8_t>(i % 251U);
    syncer::Blake3 official;
    official.update(official_input);
    CHECK(syncer::digest_hex(official.finalize()) == "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a");
}

void test_globs() {
    CHECK(syncer::glob_match("*.txt", "readme.txt"));
    CHECK(!syncer::glob_match("*.txt", "docs/readme.txt"));
    CHECK(syncer::glob_match("**/*.txt", "readme.txt"));
    CHECK(syncer::glob_match("**/*.txt", "docs/deep/readme.txt"));
    CHECK(syncer::glob_match("file?.txt", "file1.txt"));
    CHECK(!syncer::glob_match("file?.txt", "file12.txt"));
    CHECK(!syncer::glob_match("README", "readme"));
    syncer::GlobSet set({"build", "**/*.tmp"});
    CHECK(set.matches_exact("build"));
    CHECK(!set.matches_exact("build/output.bin"));
    CHECK(set.matches("build/output.bin"));
    CHECK(set.matches("build/deep/output.bin"));
    CHECK(set.matches("a/b/cache.tmp"));
    CHECK(!set.matches("a/b/cache.bin"));
}

void test_paths_and_plan() {
    CHECK(syncer::valid_relative_path("a/b.txt"));
    CHECK(!syncer::valid_relative_path("../a"));
    CHECK(!syncer::valid_relative_path("/a"));
    CHECK(!syncer::valid_relative_path("a\\b"));
    CHECK(!syncer::valid_relative_path(""));
    CHECK(!syncer::valid_relative_path("a/"));
    CHECK(!syncer::valid_relative_path("a//b"));
    CHECK(!syncer::valid_relative_path("a/./b"));
    syncer::Manifest source, destination;
    source.emplace("dir", syncer::ManifestEntry{"dir", syncer::EntryType::directory, 0, 10});
    source.emplace("dir/new.txt", syncer::ManifestEntry{"dir/new.txt", syncer::EntryType::file, 20, 11});
    source.emplace("same.bin", syncer::ManifestEntry{"same.bin", syncer::EntryType::file, 7, 12});
    destination.emplace("old.txt", syncer::ManifestEntry{"old.txt", syncer::EntryType::file, 2, 1});
    destination.emplace("same.bin", syncer::ManifestEntry{"same.bin", syncer::EntryType::file, 7, 12});
    const auto plan = syncer::make_plan(source, destination, syncer::GlobSet{}, true);
    CHECK(plan.directories.size() == 1);
    CHECK(plan.files.size() == 1);
    CHECK(plan.deletions.size() == 1);
    CHECK(plan.transfer_bytes == 20);
    CHECK(plan.directories[0].action == syncer::PlanAction::create_directory);
    CHECK(plan.directories[0].reason == "destination path is missing");
    CHECK(plan.files[0].action == syncer::PlanAction::add_file);
    CHECK(plan.files[0].reason == "destination path is missing");
    CHECK(plan.deletions[0].action == syncer::PlanAction::delete_file);
    CHECK(plan.deletions[0].reason == "path exists only at the destination and deletion is enabled");

    syncer::Manifest reason_source, reason_destination;
    reason_source.emplace("changed.bin", syncer::ManifestEntry{"changed.bin", syncer::EntryType::file, 9, 20});
    reason_destination.emplace("changed.bin", syncer::ManifestEntry{"changed.bin", syncer::EntryType::file, 8, -100});
    reason_source.emplace("was-file", syncer::ManifestEntry{"was-file", syncer::EntryType::directory, 0, 30});
    reason_destination.emplace("was-file", syncer::ManifestEntry{"was-file", syncer::EntryType::file, 4, 30});
    const auto reasons = syncer::make_plan(reason_source, reason_destination, syncer::GlobSet{}, true);
    CHECK(reasons.files.size() == 1);
    CHECK(reasons.files[0].action == syncer::PlanAction::update_file);
    CHECK(reasons.files[0].reason.find("size differs") != std::string::npos);
    CHECK(reasons.files[0].reason.find("modification time differs") != std::string::npos);
    CHECK(reasons.directories.size() == 1);
    CHECK(reasons.directories[0].action == syncer::PlanAction::replace_with_directory);
    CHECK(reasons.directories[0].reason == "destination path is a file");

    syncer::Manifest tolerance_source, tolerance_destination;
    tolerance_source.emplace("near.txt", syncer::ManifestEntry{"near.txt", syncer::EntryType::file, 5, 100});
    tolerance_source.emplace("near-dir", syncer::ManifestEntry{"near-dir", syncer::EntryType::directory, 0, 100});
    tolerance_destination.emplace("near.txt", syncer::ManifestEntry{"near.txt", syncer::EntryType::file, 5, 40});
    tolerance_destination.emplace("near-dir", syncer::ManifestEntry{"near-dir", syncer::EntryType::directory, 0, 41});
    const auto tolerated = syncer::make_plan(tolerance_source, tolerance_destination, syncer::GlobSet{}, true);
    CHECK(tolerated.files.empty());
    CHECK(tolerated.directories.empty());
    const auto exact = syncer::make_plan(tolerance_source, tolerance_destination, syncer::GlobSet{}, true, 0);
    CHECK(exact.files.size() == 1);
    CHECK(exact.directories.empty());
    CHECK(exact.files[0].reason.find("differs by 60 seconds") != std::string::npos);
    CHECK(exact.files[0].reason.find("tolerance 0 seconds") != std::string::npos);

    syncer::Manifest ordered_source, ordered_destination;
    ordered_source.emplace("a", syncer::ManifestEntry{"a", syncer::EntryType::directory, 0, 1});
    ordered_source.emplace("a/b", syncer::ManifestEntry{"a/b", syncer::EntryType::directory, 0, 1});
    ordered_destination.emplace("old", syncer::ManifestEntry{"old", syncer::EntryType::directory, 0, 1});
    ordered_destination.emplace("old/deep.bin", syncer::ManifestEntry{"old/deep.bin", syncer::EntryType::file, 1, 1});
    ordered_destination.emplace("protected", syncer::ManifestEntry{"protected", syncer::EntryType::directory, 0, 1});
    ordered_destination.emplace("protected/keep.bin", syncer::ManifestEntry{"protected/keep.bin", syncer::EntryType::file, 1, 1});
    const auto ordered = syncer::make_plan(ordered_source, ordered_destination, syncer::GlobSet({"protected"}), true);
    CHECK(ordered.directories.size() == 2);
    CHECK(ordered.directories[0].entry.path == "a");
    CHECK(ordered.directories[1].entry.path == "a/b");
    CHECK(ordered.deletions.size() == 2);
    CHECK(ordered.deletions[0].entry.path == "old/deep.bin");
    CHECK(ordered.deletions[1].entry.path == "old");
    CHECK(syncer::make_plan(ordered_source, ordered_destination, syncer::GlobSet{}, false).deletions.empty());
}

void test_protocol() {
    const auto vectors = protocol_vectors();
    syncer::ManifestEntry original{"folder/file.bin", syncer::EntryType::file, 0x100000002ULL, -123};
    syncer::ByteWriter writer;
    syncer::encode_manifest_entry(writer, original);
    const auto payload = std::move(writer).take();
    syncer::ByteReader reader(payload);
    const auto decoded = syncer::decode_manifest_entry(reader);
    reader.require_end();
    CHECK(decoded.path == original.path);
    CHECK(decoded.type == original.type);
    CHECK(decoded.size == original.size);
    CHECK(decoded.modified_seconds == original.modified_seconds);
    CHECK(bytes_hex(payload) == vectors.at("manifest_folder_file"));
    syncer::Frame frame{syncer::MessageType::scan, 42, {1, 2, 3}};
    const auto header = syncer::encode_header(frame);
    CHECK(bytes_hex(header) == vectors.at("header_scan_request_42_payload_010203"));
    std::array<std::uint8_t, 16> fixed{};
    std::copy(header.begin(), header.end(), fixed.begin());
    const auto decoded_frame = syncer::decode_frame_header(fixed);
    CHECK(decoded_frame.type == frame.type);
    CHECK(decoded_frame.request_id == 42);

    const std::array<std::uint8_t, 1> direct_placement{1};
    syncer::ByteReader placement_reader(direct_placement);
    CHECK(syncer::decode_upload_placement(placement_reader) == syncer::UploadPlacement::direct);
    placement_reader.require_end();
    CHECK(throws_syncer_error([] {
        const std::array<std::uint8_t, 1> invalid_placement{2};
        syncer::ByteReader invalid_reader(invalid_placement);
        (void)syncer::decode_upload_placement(invalid_reader);
    }));

    CHECK(throws_syncer_error([] {
        const std::array<std::uint8_t, 3> truncated{0, 0, 0};
        syncer::ByteReader short_reader(truncated);
        (void)short_reader.u32();
    }));
    CHECK(throws_syncer_error([fixed]() mutable {
        fixed[0] ^= 0xffU;
        (void)syncer::decode_frame_header(fixed);
    }));
    CHECK(throws_syncer_error([fixed]() mutable {
        fixed[12] = 0x7fU; fixed[13] = 0xffU; fixed[14] = 0xffU; fixed[15] = 0xffU;
        (void)syncer::decode_frame_header(fixed);
    }));
    CHECK(throws_syncer_error([] {
        syncer::ByteWriter invalid;
        invalid.string("../escape"); invalid.u8(static_cast<std::uint8_t>(syncer::EntryType::file));
        invalid.u64(0); invalid.i64(0);
        const auto bytes = std::move(invalid).take();
        syncer::ByteReader invalid_reader(bytes);
        (void)syncer::decode_manifest_entry(invalid_reader);
    }));

    syncer::ByteWriter plan_writer;
    plan_writer.string("SYNCER-PLAN"); plan_writer.u32(1);
    plan_writer.string("source"); plan_writer.string("/sdcard/target"); plan_writer.string("serial");
    plan_writer.u8(0); plan_writer.u8(1); plan_writer.i64(0);
    plan_writer.u32(1); plan_writer.string("**/*.tmp");
    plan_writer.u32(0);
    plan_writer.u32(1); plan_writer.u8(static_cast<std::uint8_t>(syncer::PlanAction::add_file));
    syncer::encode_manifest_entry(plan_writer, {"a.txt", syncer::EntryType::file, 3, 42});
    plan_writer.string("destination path is missing");
    plan_writer.u32(0);
    const auto plan_bytes = std::move(plan_writer).take();
    syncer::Blake3 plan_hash; plan_hash.update(plan_bytes);
    CHECK(syncer::digest_hex(plan_hash.finalize()) == vectors.at("plan_digest_reference"));
}

void test_source_scan() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("syncer-test-" + std::to_string(unique));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "nested");
    { std::ofstream(root / "nested" / "file.txt", std::ios::binary) << "data"; }
    { std::ofstream(root / std::filesystem::path(std::u8string(u8"unicode-\u03b1.txt")), std::ios::binary) << "unicode"; }
    { std::ofstream(root / "ignored.tmp", std::ios::binary) << "ignored"; }
    const auto tree = syncer::client::scan_source(root, syncer::GlobSet({"**/*.tmp"}));
    CHECK(tree.manifest.contains("nested"));
    CHECK(tree.manifest.at("nested").modified_seconds == 0);
    CHECK(tree.manifest.contains("nested/file.txt"));
    CHECK(tree.manifest.contains("unicode-\xce\xb1.txt"));
    CHECK(!tree.manifest.contains("ignored.tmp"));
    std::filesystem::create_directory(root / ".syncer");
    CHECK(throws_syncer_error([&] { (void)syncer::client::scan_source(root, syncer::GlobSet{}); }));
    std::filesystem::remove_all(root / ".syncer");
#ifndef _WIN32
    std::error_code error;
    std::filesystem::create_symlink(root / "nested" / "file.txt", root / "linked.txt", error);
    CHECK(!error);
    if (!error) {
        const auto linked = syncer::client::scan_source(root, syncer::GlobSet{});
        CHECK(linked.manifest.contains("linked.txt"));
        CHECK(linked.manifest.at("linked.txt").size == 4);
        std::filesystem::remove(root / "linked.txt");
    }
    std::filesystem::create_directory_symlink(root, root / "nested" / "loop", error);
    CHECK(!error);
    if (!error) CHECK(throws_syncer_error([&] { (void)syncer::client::scan_source(root, syncer::GlobSet{}); }));
#endif
    std::filesystem::remove_all(root);
}

#ifndef _WIN32
void test_processes() {
    const auto result = syncer::client::run_process("/bin/sh", {"-c", "printf stdout; printf stderr >&2; exit 7"});
    CHECK(result.exit_code == 7);
    CHECK(result.output == "stdoutstderr");

    auto child = syncer::client::start_process("/bin/sh", {"-c", "printf child-output"});
    CHECK(child.valid());
    child.finish(1000);
    CHECK(!child.valid());
    CHECK(child.output() == "child-output");
}
#endif
}

int main() {
    try {
        test_blake3(); test_globs(); test_paths_and_plan(); test_protocol();
        test_source_scan();
#ifndef _WIN32
        test_processes();
#endif
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n'; return 1;
    }
    if (failures != 0) { std::cerr << failures << " test(s) failed\n"; return 1; }
    std::cout << "all tests passed\n";
    return 0;
}
