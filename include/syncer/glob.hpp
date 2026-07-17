#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace syncer {

class GlobSet {
public:
    GlobSet() = default;
    explicit GlobSet(std::vector<std::string> patterns);
    [[nodiscard]] bool matches(std::string_view relative_path) const;
    [[nodiscard]] bool empty() const noexcept { return patterns_.empty(); }
private:
    std::vector<std::string> patterns_;
};

[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view path);

}
