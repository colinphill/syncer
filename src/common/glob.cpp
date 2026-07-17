#include "syncer/glob.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace syncer {

bool glob_match(std::string_view pattern, std::string_view path) {
    const auto width = path.size() + 1;
    std::vector<std::int8_t> memo((pattern.size() + 1) * width, -1);
    const auto match = [&](auto&& self, std::size_t p, std::size_t s) -> bool {
        auto& cached = memo[p * width + s];
        if (cached >= 0) return cached != 0;
        bool result = false;
        if (p == pattern.size()) {
            result = s == path.size();
        } else if (pattern[p] == '*') {
            const bool double_star = p + 1 < pattern.size() && pattern[p + 1] == '*';
            const auto next = p + (double_star ? 2 : 1);
            result = self(self, next, s);
            if (!result && double_star && next < pattern.size() && pattern[next] == '/') result = self(self, next + 1, s);
            if (!result && s < path.size() && (double_star || path[s] != '/')) result = self(self, p, s + 1);
        } else if (s < path.size() && pattern[p] == '?') {
            result = path[s] != '/' && self(self, p + 1, s + 1);
        } else if (s < path.size() && pattern[p] == path[s]) {
            result = self(self, p + 1, s + 1);
        }
        cached = result ? 1 : 0;
        return result;
    };
    return match(match, 0, 0);
}

GlobSet::GlobSet(std::vector<std::string> patterns) : patterns_(std::move(patterns)) {
    for (auto& pattern : patterns_) {
        std::replace(pattern.begin(), pattern.end(), '\\', '/');
        while (pattern.starts_with("./")) pattern.erase(0, 2);
        while (pattern.ends_with('/')) pattern.pop_back();
    }
}

bool GlobSet::matches(std::string_view relative_path) const {
    for (const auto& pattern : patterns_) {
        if (glob_match(pattern, relative_path)) return true;
        std::size_t slash = relative_path.size();
        while ((slash = relative_path.rfind('/', slash == 0 ? 0 : slash - 1)) != std::string_view::npos) {
            if (glob_match(pattern, relative_path.substr(0, slash))) return true;
            if (slash == 0) break;
        }
    }
    return false;
}

} // namespace syncer
