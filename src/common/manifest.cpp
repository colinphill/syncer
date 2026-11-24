#include "syncer/manifest.hpp"

#include <algorithm>
#include <string>

namespace syncer {
namespace {

std::string type_name(EntryType type) {
    switch (type) {
    case EntryType::file: return "file";
    case EntryType::directory: return "directory";
    case EntryType::other: return "unsupported entry";
    }
    return "unknown entry";
}

std::uint64_t mtime_difference(std::int64_t left, std::int64_t right) {
    if (left >= right) return static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right);
    return static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
}

bool mtime_differs(const ManifestEntry& source, const ManifestEntry& destination,
                   std::uint64_t tolerance_seconds) {
    return mtime_difference(source.modified_seconds, destination.modified_seconds) > tolerance_seconds;
}

std::string metadata_reason(const ManifestEntry& source, const ManifestEntry& destination,
                            std::uint64_t mtime_tolerance_seconds) {
    std::string reason;
    if (source.size != destination.size) {
        reason = "size differs (source " + std::to_string(source.size) + " bytes, destination " +
                 std::to_string(destination.size) + " bytes)";
    }
    if (mtime_differs(source, destination, mtime_tolerance_seconds)) {
        if (!reason.empty()) reason += "; ";
        reason += "modification time differs by " +
                  std::to_string(mtime_difference(source.modified_seconds, destination.modified_seconds)) +
                  " seconds (source " + std::to_string(source.modified_seconds) + ", destination " +
                  std::to_string(destination.modified_seconds) + "; tolerance " +
                  std::to_string(mtime_tolerance_seconds) + " seconds)";
    }
    return reason;
}

bool matches_daylight_saving_shift(std::uint64_t difference, std::uint64_t tolerance_seconds) {
    const auto distance = difference >= kDaylightSavingShiftSeconds
        ? difference - kDaylightSavingShiftSeconds
        : kDaylightSavingShiftSeconds - difference;
    return distance <= tolerance_seconds;
}

std::string daylight_saving_shift_reason(const ManifestEntry& source, const ManifestEntry& destination,
                                         std::uint64_t mtime_tolerance_seconds) {
    return "destination modification time differs by " +
           std::to_string(mtime_difference(source.modified_seconds, destination.modified_seconds)) +
           " seconds (source " + std::to_string(source.modified_seconds) + ", destination " +
           std::to_string(destination.modified_seconds) +
           "), matching a one-hour daylight-saving shift within the tolerance of " +
           std::to_string(mtime_tolerance_seconds) + " seconds";
}

constexpr const char* kUntrackedTimeReason =
    "destination modification time is not recorded in the timestamp catalog";

PlanAction deletion_action(EntryType type) {
    switch (type) {
    case EntryType::file: return PlanAction::delete_file;
    case EntryType::directory: return PlanAction::delete_directory;
    case EntryType::other: return PlanAction::delete_other;
    }
    return PlanAction::delete_other;
}

} // namespace

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
                   const GlobSet& exclusions, bool delete_extras,
                   std::uint64_t mtime_tolerance_seconds,
                   const PathSet& untracked_destination_times) {
    SyncPlan plan;
    for (const auto& [path, source_entry] : source) {
        const auto found = destination.find(path);
        const bool missing = found == destination.end();
        const bool type_changed = !missing && found->second.type != source_entry.type;
        if (source_entry.type == EntryType::directory) {
            if (missing) {
                plan.directories.push_back({source_entry, PlanAction::create_directory,
                                            "destination path is missing"});
            } else if (type_changed) {
                plan.directories.push_back({source_entry, PlanAction::replace_with_directory,
                                            "destination path is a " + type_name(found->second.type)});
            }
        } else if (source_entry.type == EntryType::file) {
            if (missing) {
                plan.files.push_back({source_entry, PlanAction::add_file, "destination path is missing"});
                plan.transfer_bytes += source_entry.size;
            } else if (type_changed) {
                plan.files.push_back({source_entry, PlanAction::replace_with_file,
                                      "destination path is a " + type_name(found->second.type)});
                plan.transfer_bytes += source_entry.size;
            } else {
                const auto& destination_entry = found->second;
                const bool untracked = untracked_destination_times.contains(path);
                const auto difference =
                    mtime_difference(source_entry.modified_seconds, destination_entry.modified_seconds);
                if (destination_entry.size != source_entry.size ||
                    (difference > mtime_tolerance_seconds &&
                     !(untracked && matches_daylight_saving_shift(difference, mtime_tolerance_seconds)))) {
                    plan.files.push_back({source_entry, PlanAction::update_file,
                                          metadata_reason(source_entry, destination_entry,
                                                          mtime_tolerance_seconds)});
                    plan.transfer_bytes += source_entry.size;
                } else if (untracked) {
                    plan.timestamps.push_back({source_entry, PlanAction::record_timestamp,
                        difference > mtime_tolerance_seconds
                            ? daylight_saving_shift_reason(source_entry, destination_entry,
                                                           mtime_tolerance_seconds)
                            : std::string(kUntrackedTimeReason)});
                }
            }
        }
    }
    if (delete_extras) {
        for (const auto& [path, destination_entry] : destination) {
            if (!source.contains(path) && !exclusions.matches(path)) {
                plan.deletions.push_back({destination_entry, deletion_action(destination_entry.type),
                                          "path exists only at the destination and deletion is enabled"});
            }
        }
    }
    std::sort(plan.directories.begin(), plan.directories.end(), [](const auto& a, const auto& b) {
        return path_depth(a.entry.path) < path_depth(b.entry.path) ||
               (path_depth(a.entry.path) == path_depth(b.entry.path) && a.entry.path < b.entry.path);
    });
    std::sort(plan.files.begin(), plan.files.end(), [](const auto& a, const auto& b) {
        return a.entry.path < b.entry.path;
    });
    std::sort(plan.timestamps.begin(), plan.timestamps.end(), [](const auto& a, const auto& b) {
        return a.entry.path < b.entry.path;
    });
    std::sort(plan.deletions.begin(), plan.deletions.end(), [](const auto& a, const auto& b) {
        return path_depth(a.entry.path) > path_depth(b.entry.path) ||
               (path_depth(a.entry.path) == path_depth(b.entry.path) && a.entry.path > b.entry.path);
    });
    return plan;
}

} // namespace syncer
