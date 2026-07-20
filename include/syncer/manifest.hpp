#pragma once

#include "syncer/glob.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace syncer {

enum class EntryType : std::uint8_t { file = 1, directory = 2, other = 3 };

struct ManifestEntry {
    std::string path;
    EntryType type{EntryType::file};
    std::uint64_t size{};
    std::int64_t modified_seconds{};
    bool operator==(const ManifestEntry&) const = default;
};

using Manifest = std::unordered_map<std::string, ManifestEntry>;

enum class PlanAction {
    create_directory,
    replace_with_directory,
    add_file,
    update_file,
    replace_with_file,
    delete_file,
    delete_directory,
    delete_other
};

struct PlannedChange {
    ManifestEntry entry;
    PlanAction action{};
    std::string reason;
};

struct SyncPlan {
    std::vector<PlannedChange> directories;
    std::vector<PlannedChange> files;
    std::vector<PlannedChange> deletions;
    std::uint64_t transfer_bytes{};
};

struct DestinationPrecondition {
    std::string path;
    bool exists{};
    ManifestEntry entry;
};

[[nodiscard]] SyncPlan make_plan(const Manifest& source, const Manifest& destination,
                                 const GlobSet& exclusions, bool delete_extras,
                                 std::uint64_t mtime_tolerance_seconds = 60);
[[nodiscard]] bool valid_relative_path(std::string_view path);
[[nodiscard]] std::size_t path_depth(std::string_view path);

}
