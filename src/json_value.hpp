#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kirakara::show::detail::json {

struct Value {
    using Array = std::vector<Value>;
    using Object = std::unordered_map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, double,
        std::string, Array, Object>;

    Storage storage{nullptr};

    [[nodiscard]] const bool* boolean() const noexcept;
    [[nodiscard]] const double* number() const noexcept;
    [[nodiscard]] const std::string* string() const noexcept;
    [[nodiscard]] const Array* array() const noexcept;
    [[nodiscard]] const Object* object() const noexcept;
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
};

[[nodiscard]] std::optional<Value> parse(std::string_view text);

// Parses text while skipping (not materializing) the values of the listed
// object keys. Keys are matched at every nesting level, so callers can drop
// large payloads they never read (e.g. prelude background images) without
// paying to decode/allocate them into the tree.
[[nodiscard]] std::optional<Value> parse(
    std::string_view text,
    const std::vector<std::string>& skip_keys);

} // namespace kirakara::show::detail::json
