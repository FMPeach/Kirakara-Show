#include "json_value.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace kirakara::show::detail::json {
namespace {

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    explicit Parser(
        std::string_view text,
        const std::vector<std::string>& skip_keys)
        : text_(text), skip_keys_(&skip_keys) {}

    std::optional<Value> parse_document() {
        skip_space();
        auto value = parse_value(0);
        skip_space();
        if (!value || cursor_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    static constexpr std::size_t kMaxDepth = 128;

    void skip_space() {
        while (cursor_ < text_.size()
                && std::isspace(static_cast<unsigned char>(text_[cursor_]))) {
            ++cursor_;
        }
    }

    bool consume(char value) {
        skip_space();
        if (cursor_ >= text_.size() || text_[cursor_] != value) return false;
        ++cursor_;
        return true;
    }

    bool consume_literal(std::string_view value) {
        if (text_.substr(cursor_, value.size()) != value) return false;
        cursor_ += value.size();
        return true;
    }

    std::optional<std::uint32_t> parse_hex_quad() {
        if (cursor_ + 4 > text_.size()) return std::nullopt;
        std::uint32_t result{};
        for (int index = 0; index < 4; ++index) {
            const auto digit = hex_digit(text_[cursor_++]);
            if (digit < 0) return std::nullopt;
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        return result;
    }

    std::optional<std::string> parse_string() {
        skip_space();
        if (cursor_ >= text_.size() || text_[cursor_] != '"') {
            return std::nullopt;
        }
        ++cursor_;
        std::string output;
        while (cursor_ < text_.size()) {
            const auto current = static_cast<unsigned char>(text_[cursor_++]);
            if (current == '"') return output;
            if (current < 0x20U) return std::nullopt;
            if (current != '\\') {
                output.push_back(static_cast<char>(current));
                continue;
            }
            if (cursor_ >= text_.size()) return std::nullopt;
            const auto escaped = text_[cursor_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                auto codepoint = parse_hex_quad();
                if (!codepoint) return std::nullopt;
                if (*codepoint >= 0xd800U && *codepoint <= 0xdbffU) {
                    if (cursor_ + 2 > text_.size()
                            || text_[cursor_] != '\\'
                            || text_[cursor_ + 1] != 'u') {
                        return std::nullopt;
                    }
                    cursor_ += 2;
                    const auto low = parse_hex_quad();
                    if (!low || *low < 0xdc00U || *low > 0xdfffU) {
                        return std::nullopt;
                    }
                    codepoint = 0x10000U
                        + ((*codepoint - 0xd800U) << 10U)
                        + (*low - 0xdc00U);
                } else if (*codepoint >= 0xdc00U && *codepoint <= 0xdfffU) {
                    return std::nullopt;
                }
                append_utf8(output, *codepoint);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parse_number() {
        const auto begin = cursor_;
        if (cursor_ < text_.size() && text_[cursor_] == '-') ++cursor_;
        if (cursor_ >= text_.size()) return std::nullopt;
        if (text_[cursor_] == '0') {
            ++cursor_;
        } else if (text_[cursor_] >= '1' && text_[cursor_] <= '9') {
            while (cursor_ < text_.size()
                    && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
        } else {
            return std::nullopt;
        }
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            const auto fraction = cursor_;
            while (cursor_ < text_.size()
                    && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
            if (fraction == cursor_) return std::nullopt;
        }
        if (cursor_ < text_.size()
                && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size()
                    && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
                ++cursor_;
            }
            const auto exponent = cursor_;
            while (cursor_ < text_.size()
                    && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
            if (exponent == cursor_) return std::nullopt;
        }
        const std::string token{text_.substr(begin, cursor_ - begin)};
        char* end{};
        const auto number = std::strtod(token.c_str(), &end);
        if (!end || end != token.c_str() + token.size()
                || !std::isfinite(number)) {
            return std::nullopt;
        }
        return Value{number};
    }

    std::optional<Value> parse_array(std::size_t depth) {
        if (!consume('[')) return std::nullopt;
        Value::Array values;
        skip_space();
        if (consume(']')) return Value{std::move(values)};
        while (true) {
            auto value = parse_value(depth + 1);
            if (!value) return std::nullopt;
            values.push_back(std::move(*value));
            skip_space();
            if (consume(']')) return Value{std::move(values)};
            if (!consume(',')) return std::nullopt;
        }
    }

    bool should_skip_key(const std::string& key) const {
        if (!skip_keys_) return false;
        for (const auto& candidate : *skip_keys_) {
            if (candidate == key) return true;
        }
        return false;
    }

    // Advances over a JSON string without materializing it. Handles escapes
    // and \uXXXX (including surrogate pairs) so the cursor lands correctly.
    bool scan_string() {
        skip_space();
        if (cursor_ >= text_.size() || text_[cursor_] != '"') return false;
        ++cursor_;
        while (cursor_ < text_.size()) {
            const auto current = static_cast<unsigned char>(text_[cursor_++]);
            if (current == '"') return true;
            if (current < 0x20U) return false;
            if (current != '\\') continue;
            if (cursor_ >= text_.size()) return false;
            const auto escaped = text_[cursor_++];
            if (escaped != 'u') continue;
            if (cursor_ + 4 > text_.size()) return false;
            cursor_ += 4;
            if (cursor_ + 2 <= text_.size() && text_[cursor_] == '\\'
                    && text_[cursor_ + 1] == 'u') {
                cursor_ += 2;
                if (cursor_ + 4 > text_.size()) return false;
                cursor_ += 4;
            }
        }
        return false;
    }

    bool skip_array(std::size_t depth) {
        if (!consume('[')) return false;
        skip_space();
        if (consume(']')) return true;
        while (true) {
            if (!skip_value(depth + 1)) return false;
            skip_space();
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    bool skip_object(std::size_t depth) {
        if (!consume('{')) return false;
        skip_space();
        if (consume('}')) return true;
        while (true) {
            if (!scan_string()) return false;
            if (!consume(':')) return false;
            if (!skip_value(depth + 1)) return false;
            skip_space();
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    bool skip_value(std::size_t depth) {
        if (depth > kMaxDepth) return false;
        skip_space();
        if (cursor_ >= text_.size()) return false;
        switch (text_[cursor_]) {
        case '{': return skip_object(depth);
        case '[': return skip_array(depth);
        case '"': return scan_string();
        case 't': return consume_literal("true");
        case 'f': return consume_literal("false");
        case 'n': return consume_literal("null");
        default: {
            const auto begin = cursor_;
            while (cursor_ < text_.size()
                    && (std::isdigit(static_cast<unsigned char>(text_[cursor_]))
                        || text_[cursor_] == '-' || text_[cursor_] == '+'
                        || text_[cursor_] == '.' || text_[cursor_] == 'e'
                        || text_[cursor_] == 'E')) {
                ++cursor_;
            }
            return cursor_ > begin;
        }
        }
    }

    std::optional<Value> parse_object(std::size_t depth) {
        if (!consume('{')) return std::nullopt;
        Value::Object values;
        skip_space();
        if (consume('}')) return Value{std::move(values)};
        while (true) {
            auto key = parse_string();
            if (!key || !consume(':')) return std::nullopt;
            if (should_skip_key(*key)) {
                if (!skip_value(depth + 1)) return std::nullopt;
            } else {
                auto value = parse_value(depth + 1);
                if (!value) return std::nullopt;
                values.insert_or_assign(std::move(*key), std::move(*value));
            }
            skip_space();
            if (consume('}')) return Value{std::move(values)};
            if (!consume(',')) return std::nullopt;
        }
    }

    std::optional<Value> parse_value(std::size_t depth) {
        if (depth > kMaxDepth) return std::nullopt;
        skip_space();
        if (cursor_ >= text_.size()) return std::nullopt;
        switch (text_[cursor_]) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': {
            auto value = parse_string();
            return value ? std::optional<Value>{Value{std::move(*value)}}
                         : std::nullopt;
        }
        case 't':
            if (consume_literal("true")) return Value{true};
            return std::nullopt;
        case 'f':
            if (consume_literal("false")) return Value{false};
            return std::nullopt;
        case 'n':
            if (consume_literal("null")) return Value{nullptr};
            return std::nullopt;
        default:
            return parse_number();
        }
    }

    std::string_view text_;
    std::size_t cursor_{};
    const std::vector<std::string>* skip_keys_{};
};

} // namespace

const bool* Value::boolean() const noexcept {
    return std::get_if<bool>(&storage);
}

const double* Value::number() const noexcept {
    return std::get_if<double>(&storage);
}

const std::string* Value::string() const noexcept {
    return std::get_if<std::string>(&storage);
}

const Value::Array* Value::array() const noexcept {
    return std::get_if<Array>(&storage);
}

const Value::Object* Value::object() const noexcept {
    return std::get_if<Object>(&storage);
}

const Value* Value::find(std::string_view key) const noexcept {
    const auto* members = object();
    if (!members) return nullptr;
    const auto found = members->find(std::string{key});
    return found == members->end() ? nullptr : &found->second;
}

std::optional<Value> parse(std::string_view text) {
    return Parser{text}.parse_document();
}

std::optional<Value> parse(
    std::string_view text,
    const std::vector<std::string>& skip_keys) {
    return Parser{text, skip_keys}.parse_document();
}

} // namespace kirakara::show::detail::json
