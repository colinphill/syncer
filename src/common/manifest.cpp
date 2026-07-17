#include "syncer/manifest.hpp"

#include <algorithm>

namespace syncer {

bool valid_relative_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != path.npos ||
        path.find('\0') != path.npos) return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == path.npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == path.npos) break;
        start = end + 1;
    }
    return true;
}

std::size_t path_depth(std::string_view path) {
    return 1 + static_cast<std::size_t>(std::count(path.begin(), path.end(), '/'));
}

SyncPlan make_plan(const Manifest& source, const Manifest& destination,
                   const GlobSet& exclusions, bool delete_extras) {
    SyncPlan plan;
    for (const auto& [path, source_entry] : source) {
        const auto found = destination.find(path);
        const bool missing = found == destination.end();
        const bool type_changed = !missing && found->second.type != source_entry.type;
        if (source_entry.type == EntryType::directory) {
            if (missing || type_changed || found->second.modified_seconds != source_entry.modified_seconds)
                plan.directories.push_back(source_entry);
        } else if (source_entry.type == EntryType::file) {
            const bool content_changed = missing || type_changed || found->second.size != source_entry.size ||
                                         found->second.modified_seconds != source_entry.modified_seconds;
            if (content_changed) {
                plan.files.push_back(source_entry);
                plan.transfer_bytes += source_entry.size;
            }
        }
    }
    if (delete_extras) {
        for (const auto& [path, destination_entry] : destination) {
            if (!source.contains(path) && !exclusions.matches(path)) plan.deletions.push_back(destination_entry);
        }
    }
    std::sort(plan.directories.begin(), plan.directories.end(), [](const auto& a, const auto& b) {
        return path_depth(a.path) < path_depth(b.path) ||
               (path_depth(a.path) == path_depth(b.path) && a.path < b.path);
    });
    std::sort(plan.files.begin(), plan.files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    std::sort(plan.deletions.begin(), plan.deletions.end(), [](const auto& a, const auto& b) {
        return path_depth(a.path) > path_depth(b.path) ||
               (path_depth(a.path) == path_depth(b.path) && a.path > b.path);
    });
    return plan;
}

} // namespace syncer
