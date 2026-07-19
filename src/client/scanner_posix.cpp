#include "scanner.hpp"

#include "syncer/error.hpp"
#include "syncer/version.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <sys/stat.h>
#include <vector>

namespace syncer::client {
namespace fs = std::filesystem;
namespace {

std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

struct FileFacts {
    mode_t mode{};
    std::uint64_t size{};
    std::int64_t modified{};
    dev_t device{};
    ino_t inode{};
};

FileFacts query_facts(const fs::path& path) {
    struct stat data {};
    if (stat(path.c_str(), &data) != 0)
        throw Error(ExitCode::safety, "cannot inspect source entry: " + utf8(path) + ": " + std::strerror(errno));
    return {data.st_mode, static_cast<std::uint64_t>(data.st_size),
            static_cast<std::int64_t>(data.st_mtime), data.st_dev, data.st_ino};
}

bool is_symbolic_link(const fs::path& path) {
    struct stat data {};
    if (lstat(path.c_str(), &data) != 0)
        throw Error(ExitCode::safety, "cannot inspect source entry: " + utf8(path) + ": " + std::strerror(errno));
    return S_ISLNK(data.st_mode);
}

struct DirectoryIdentity {
    dev_t device{};
    ino_t inode{};
    bool operator==(const DirectoryIdentity&) const = default;
};

} // namespace

std::int64_t source_modified_seconds(const fs::path& path) { return query_facts(path).modified; }

SourceTree scan_source(const fs::path& requested_root, const GlobSet& exclusions) {
    std::error_code error;
    const auto root = fs::canonical(requested_root, error);
    if (error || !fs::is_directory(root)) throw Error(ExitCode::safety, "source must be an accessible directory");
    SourceTree tree;
    tree.manifest.reserve(4096);
    tree.files.reserve(4096);
    std::vector<DirectoryIdentity> ancestors;

    std::function<void(const fs::path&, const std::string&, bool)> walk;
    walk = [&](const fs::path& physical, const std::string& logical, bool is_root) {
        if (!is_root) {
            if (path_depth(logical) == 1 && logical == kReservedDirectory)
                throw Error(ExitCode::safety, "source contains the reserved top-level .syncer path");
            if (exclusions.matches_exact(logical)) return;
        }

        fs::path effective = physical;
        if (is_symbolic_link(physical)) {
            effective = fs::canonical(physical, error);
            if (error) throw Error(ExitCode::safety, "source contains a broken symbolic link: " + utf8(physical));
        }
        const auto facts = query_facts(effective);
        if (S_ISDIR(facts.mode)) {
            const DirectoryIdentity identity{facts.device, facts.inode};
            if (std::find(ancestors.begin(), ancestors.end(), identity) != ancestors.end())
                throw Error(ExitCode::safety, "source contains a recursive symbolic link at: " + logical);
            ancestors.push_back(identity);
            if (!is_root) tree.manifest.emplace(logical, ManifestEntry{logical, EntryType::directory, 0, 0});

            fs::directory_iterator iterator(effective, error);
            if (error) {
                ancestors.pop_back();
                throw Error(ExitCode::safety, "cannot enumerate source directory: " + utf8(effective));
            }
            const fs::directory_iterator end;
            try {
                while (iterator != end) {
                    const auto child_path = iterator->path();
                    const auto name = utf8(child_path.filename());
                    const auto child_logical = logical.empty() ? name : logical + "/" + name;
                    walk(child_path, child_logical, false);
                    iterator.increment(error);
                    if (error) throw Error(ExitCode::safety, "cannot enumerate source directory: " + utf8(effective));
                }
            } catch (...) {
                ancestors.pop_back();
                throw;
            }
            ancestors.pop_back();
        } else if (S_ISREG(facts.mode)) {
            ManifestEntry entry{logical, EntryType::file, facts.size, facts.modified};
            tree.manifest.emplace(logical, entry);
            tree.files.emplace(logical, SourceFile{entry, effective});
        } else {
            throw Error(ExitCode::safety, "source contains an unsupported file type: " + logical);
        }
    };
    walk(root, {}, true);
    return tree;
}

} // namespace syncer::client
