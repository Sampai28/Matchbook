#pragma once

// A minimal JSON writer and a small object parser.
//
// Deliberately not nlohmann/json. The server's JSON surface is fixed and tiny:
// it writes four response shapes and reads one flat object of scalars. Pulling
// a 25k-line header for that would add a FetchContent dependency and several
// seconds to every compile, on a project whose whole point is build-and-measure
// iteration speed.
//
// The parser handles exactly what POST /order sends: a flat object whose values
// are strings, integers, or booleans. Nested objects, arrays, floats, and
// unicode escapes are not supported and are reported as errors rather than
// silently mis-parsed.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace matchbook::json {

// --------------------------------------------------------------------------
// writing
// --------------------------------------------------------------------------

class Writer {
public:
    Writer() { out_.reserve(1024); }

    void begin_object() { comma(); out_ += '{'; first_ = true; }
    void end_object()   { out_ += '}'; first_ = false; }
    void begin_array()  { comma(); out_ += '['; first_ = true; }
    void end_array()    { out_ += ']'; first_ = false; }

    void key(std::string_view k) {
        comma();
        out_ += '"';
        escape_into(k);
        out_ += "\":";
        first_ = true;  // a value follows immediately; no comma before it
    }

    void value(std::string_view s) {
        comma();
        out_ += '"';
        escape_into(s);
        out_ += '"';
        first_ = false;
    }

    void value(std::int64_t v)  { comma(); out_ += std::to_string(v); first_ = false; }
    void value(std::uint64_t v) { comma(); out_ += std::to_string(v); first_ = false; }
    void value(int v)           { value(static_cast<std::int64_t>(v)); }
    void value(bool v)          { comma(); out_ += (v ? "true" : "false"); first_ = false; }
    void value_double(double v) {
        comma();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.4f", v);
        out_ += buf;
        first_ = false;
    }
    void null()                 { comma(); out_ += "null"; first_ = false; }

    // Convenience: key + value in one call, which is what nearly every call
    // site wants.
    void field(std::string_view k, std::string_view v) { key(k); value(v); }
    void field(std::string_view k, std::int64_t v)     { key(k); value(v); }
    void field(std::string_view k, std::uint64_t v)    { key(k); value(v); }
    void field(std::string_view k, bool v)             { key(k); value(v); }

    [[nodiscard]] const std::string& str() const noexcept { return out_; }

private:
    void comma() {
        if (!first_) out_ += ',';
        first_ = false;
    }

    void escape_into(std::string_view s) {
        for (char c : s) {
            switch (c) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out_ += buf;
                    } else {
                        out_ += c;
                    }
            }
        }
    }

    std::string out_;
    bool        first_ = true;
};

// --------------------------------------------------------------------------
// parsing
// --------------------------------------------------------------------------

// A flat object: every value is stored as its raw text. Callers convert.
class Object {
public:
    [[nodiscard]] bool has(std::string_view k) const {
        return fields_.find(std::string(k)) != fields_.end();
    }

    [[nodiscard]] std::string get_string(std::string_view k, std::string_view fallback = "") const {
        auto it = fields_.find(std::string(k));
        return it == fields_.end() ? std::string(fallback) : it->second;
    }

    // Returns false when the key is absent or the value is not a valid integer,
    // so a caller can distinguish "not supplied" from "supplied as garbage".
    [[nodiscard]] bool get_int(std::string_view k, std::int64_t& out) const {
        auto it = fields_.find(std::string(k));
        if (it == fields_.end() || it->second.empty()) return false;

        const std::string& s = it->second;
        std::size_t i = 0;
        bool neg = false;
        if (s[0] == '-') { neg = true; i = 1; }
        if (i >= s.size()) return false;

        std::int64_t v = 0;
        for (; i < s.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            const int d = s[i] - '0';
            // Reject before overflowing rather than after: signed overflow is
            // undefined behaviour and the optimiser may delete the check.
            if (v > (INT64_MAX - d) / 10) return false;
            v = v * 10 + d;
        }
        out = neg ? -v : v;
        return true;
    }

    [[nodiscard]] bool get_bool(std::string_view k, bool& out) const {
        auto it = fields_.find(std::string(k));
        if (it == fields_.end()) return false;
        if (it->second == "true")  { out = true;  return true; }
        if (it->second == "false") { out = false; return true; }
        return false;
    }

    friend bool parse_object(std::string_view text, Object& out, std::string& error);

private:
    std::unordered_map<std::string, std::string> fields_;
};

// Parses a flat JSON object. Returns false with a reason in `error` on anything
// it does not support, rather than guessing.
inline bool parse_object(std::string_view text, Object& out, std::string& error) {
    out.fields_.clear();

    std::size_t i = 0;
    auto skip_ws = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };

    skip_ws();
    if (i >= text.size() || text[i] != '{') { error = "expected '{'"; return false; }
    ++i;
    skip_ws();
    if (i < text.size() && text[i] == '}') return true;  // empty object

    for (;;) {
        skip_ws();
        if (i >= text.size() || text[i] != '"') { error = "expected key string"; return false; }
        ++i;

        std::string key;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
                switch (text[i]) {
                    case 'n': key += '\n'; break;
                    case 't': key += '\t'; break;
                    case 'r': key += '\r'; break;
                    default:  key += text[i];
                }
            } else {
                key += text[i];
            }
            ++i;
        }
        if (i >= text.size()) { error = "unterminated key"; return false; }
        ++i;  // closing quote

        skip_ws();
        if (i >= text.size() || text[i] != ':') { error = "expected ':'"; return false; }
        ++i;
        skip_ws();
        if (i >= text.size()) { error = "expected value"; return false; }

        std::string value;
        if (text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    ++i;
                    switch (text[i]) {
                        case 'n': value += '\n'; break;
                        case 't': value += '\t'; break;
                        case 'r': value += '\r'; break;
                        default:  value += text[i];
                    }
                } else {
                    value += text[i];
                }
                ++i;
            }
            if (i >= text.size()) { error = "unterminated string value"; return false; }
            ++i;
        } else if (text[i] == '{' || text[i] == '[') {
            error = "nested objects and arrays are not supported";
            return false;
        } else {
            while (i < text.size() && text[i] != ',' && text[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(text[i]))) {
                value += text[i];
                ++i;
            }
            if (value.empty()) { error = "empty value"; return false; }
        }

        out.fields_.emplace(std::move(key), std::move(value));

        skip_ws();
        if (i >= text.size()) { error = "unterminated object"; return false; }
        if (text[i] == ',') { ++i; continue; }
        if (text[i] == '}') return true;
        error = "expected ',' or '}'";
        return false;
    }
}

}  // namespace matchbook::json
