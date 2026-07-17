#pragma once

#include "syncer/glob.hpp"

#include <cstdint>
#include <map>
#include <string>
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

using Manifest = std::map<std::string, ManifestEntry, std::less<>>;

struct SyncPlan {
    std::vector<ManifestEntry> directories;
    std::vector<ManifestEntry> files;
    std::vector<ManifestEntry> deletions;
    std::uint64_t transfer_bytes{};
};

[[nodiscard]] SyncPlan make_plan(const Manifest& source, const Manifest& destination,
                                 const GlobSet& exclusions, bool delete_extras);
[[nodiscard]] bool valid_relative_path(std::string_view path);
[[nodiscard]] std::size_t path_depth(std::string_view path);

}
