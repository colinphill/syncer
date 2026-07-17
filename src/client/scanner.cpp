#include "scanner.hpp"

#include "syncer/error.hpp"
#include "syncer/version.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <functional>
#include <windows.h>
#include <winioctl.h>

namespace syncer::client {
namespace fs = std::filesystem;
namespace {

std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

struct FileFacts {
    DWORD attributes{};
    std::uint64_t size{};
    FILETIME modified{};
    DWORD reparse_tag{};
};

std::int64_t modified_seconds(const FILETIME& modified) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = modified.dwLowDateTime;
    ticks.HighPart = modified.dwHighDateTime;
    constexpr std::uint64_t windows_to_unix_ticks = 116444736000000000ULL;
    if (ticks.QuadPart < windows_to_unix_ticks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - windows_to_unix_ticks) / 10000000ULL);
}

FileFacts facts_from_find(const WIN32_FIND_DATAW& data) {
    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return FileFacts{data.dwFileAttributes, size.QuadPart, data.ftLastWriteTime, data.dwReserved0};
}

FileFacts query_facts(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        throw Error(ExitCode::safety, "cannot inspect source entry: " + utf8(path));
    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return FileFacts{data.dwFileAttributes, size.QuadPart, data.ftLastWriteTime, 0};
}

std::wstring identity(const fs::path& path) {
    auto normalized = path.lexically_normal().wstring();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return normalized;
}

void validate_reparse_point(const fs::path& path, DWORD known_tag) {
    if (known_tag == IO_REPARSE_TAG_SYMLINK || known_tag == IO_REPARSE_TAG_MOUNT_POINT) return;
    const HANDLE handle = CreateFileW(path.c_str(), 0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw Error(ExitCode::safety, "cannot inspect source reparse point: " + utf8(path));
    std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> buffer{};
    DWORD returned{};
    const BOOL success = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(handle);
    if (!success || returned < sizeof(ULONG))
        throw Error(ExitCode::safety, "cannot inspect source reparse point: " + utf8(path));
    ULONG tag = known_tag;
    if (tag == 0) std::memcpy(&tag, buffer.data(), sizeof(tag));
    if (tag != IO_REPARSE_TAG_SYMLINK && tag != IO_REPARSE_TAG_MOUNT_POINT)
        throw Error(ExitCode::safety, "source contains an unsupported reparse point: " + utf8(path));
}

} // namespace

std::int64_t source_modified_seconds(const fs::path& path) {
    return modified_seconds(query_facts(path).modified);
}

SourceTree scan_source(const fs::path& requested_root, const GlobSet& exclusions) {
    std::error_code error;
    const auto root = fs::canonical(requested_root, error);
    if (error || !fs::is_directory(root)) throw Error(ExitCode::safety, "source must be an accessible directory");
    SourceTree tree;
    tree.manifest.reserve(4096);
    tree.files.reserve(4096);
    std::vector<std::wstring> ancestors;

    std::function<void(const fs::path&, const std::string&, bool, FileFacts)> walk;
    walk = [&](const fs::path& physical, const std::string& logical, bool is_root, FileFacts facts) {
        if (!is_root) {
            if (path_depth(logical) == 1 && logical == kReservedDirectory)
                throw Error(ExitCode::safety, "source contains the reserved top-level .syncer path");
            // Excluded directories are pruned, so their descendants never reach
            // this call. Matching ancestors again for every file is redundant.
            if (exclusions.matches_exact(logical)) return;
        }
        fs::path effective = physical;
        if ((facts.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            validate_reparse_point(physical, facts.reparse_tag);
            effective = fs::canonical(physical, error);
            if (error) throw Error(ExitCode::safety, "source contains a broken symbolic link: " + utf8(physical));
            facts = query_facts(effective);
        }
        if ((facts.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const auto key = identity(effective);
            if (std::find(ancestors.begin(), ancestors.end(), key) != ancestors.end())
                throw Error(ExitCode::safety, "source contains a recursive symbolic link or junction at: " + logical);
            ancestors.push_back(key);
            if (!is_root) {
                ManifestEntry entry{logical, EntryType::directory, 0, 0};
                tree.manifest.emplace(logical, entry);
            }
            WIN32_FIND_DATAW child_data{};
            const auto pattern = effective / L"*";
            HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &child_data,
                                             FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
                search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &child_data,
                                          FindExSearchNameMatch, nullptr, 0);
            if (search == INVALID_HANDLE_VALUE) {
                ancestors.pop_back();
                throw Error(ExitCode::safety, "cannot enumerate source directory: " + utf8(effective));
            }
            try {
                do {
                    const std::wstring_view wide_name(child_data.cFileName);
                    if (wide_name == L"." || wide_name == L"..") continue;
                    const fs::path child_path = effective / child_data.cFileName;
                    const auto name = utf8(fs::path(child_data.cFileName));
                    const auto child_logical = logical.empty() ? name : logical + "/" + name;
                    walk(child_path, child_logical, false, facts_from_find(child_data));
                } while (FindNextFileW(search, &child_data));
                if (GetLastError() != ERROR_NO_MORE_FILES)
                    throw Error(ExitCode::safety, "cannot enumerate source directory: " + utf8(effective));
            } catch (...) {
                FindClose(search);
                ancestors.pop_back();
                throw;
            }
            FindClose(search);
            ancestors.pop_back();
        } else {
            ManifestEntry entry{logical, EntryType::file, facts.size, modified_seconds(facts.modified)};
            tree.manifest.emplace(logical, entry);
            tree.files.emplace(logical, SourceFile{entry, effective});
        }
    };
    walk(root, {}, true, query_facts(root));
    return tree;
}

} // namespace syncer::client
