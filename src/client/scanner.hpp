#pragma once

#include "syncer/manifest.hpp"

#include <filesystem>
#include <unordered_map>

namespace syncer::client {

struct SourceFile {
    ManifestEntry entry;
    std::filesystem::path physical_path;
};

struct SourceTree {
    Manifest manifest;
    std::unordered_map<std::string, SourceFile> files;
};

[[nodiscard]] SourceTree scan_source(const std::filesystem::path& root, const GlobSet& exclusions);
[[nodiscard]] std::int64_t source_modified_seconds(const std::filesystem::path& path);

}
