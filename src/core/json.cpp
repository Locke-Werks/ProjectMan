#include "json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pm::json {
namespace {

// Deep enough for any MCP message, shallow enough that a hostile document
// cannot walk the stack off the end.
constexpr int kMaxDepth = 64;

class Parser {
public:
    Parser(std::string_view text, std::string* error) : s_(text), error_(error) {}

    bool run(Value* out)
    {
        skipWs();
        if (!parseValue(out, 0))
            return false;
        skipWs();
        if (i_ != s_.size())
            return fail("trailing data after the document");
        return true;
    }

private:
    bool fail(const char* why)
    {
        if (error_)
            *error_ = std::string(why) + " at offset " + std::to_string(i_);
        return false;
    }

    bool eof() const { return i_ >= s_.size(); }
    char peek() const { return s_[i_]; }

    void skipWs()
    {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++i_;
            else
                break;
        }
    }

    bool literal(std::string_view word)
    {
        if (s_.compare(i_, word.size(), word) != 0)
            return false;
        i_ += word.size();
        return true;
    }

    bool parseValue(Value* out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("nested too deeply");
        if (eof())
            return fail("unexpected end of document");

        switch (peek()) {
        case 'n':
            if (!literal("null"))
                return fail("expected null");
            *out = makeNull();
            return true;
        case 't':
            if (!literal("true"))
                return fail("expected true");
            *out = makeBool(true);
            return true;
        case 'f':
            if (!literal("false"))
                return fail("expected false");
            *out = makeBool(false);
            return true;
        case '"': {
            std::string str;
            if (!parseString(&str))
                return false;
            *out = makeString(std::move(str));
            return true;
        }
        case '[': return parseArray(out, depth);
        case '{': return parseObject(out, depth);
        default:  return parseNumber(out);
        }
    }

    bool parseArray(Value* out, int depth)
    {
        ++i_;   // [
        Value v;
        v.type = Value::Type::Array;

        skipWs();
        if (!eof() && peek() == ']') {
            ++i_;
            *out = std::move(v);
            return true;
        }

        for (;;) {
            skipWs();
            Value item;
            if (!parseValue(&item, depth + 1))
                return false;
            v.array.push_back(std::move(item));

            skipWs();
            if (eof())
                return fail("unterminated array");
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == ']') {
                ++i_;
                *out = std::move(v);
                return true;
            }
            return fail("expected , or ] in array");
        }
    }

    bool parseObject(Value* out, int depth)
    {
        ++i_;   // {
        Value v;
        v.type = Value::Type::Object;

        skipWs();
        if (!eof() && peek() == '}') {
            ++i_;
            *out = std::move(v);
            return true;
        }

        for (;;) {
            skipWs();
            if (eof() || peek() != '"')
                return fail("expected a quoted key in object");

            std::string key;
            if (!parseString(&key))
                return false;

            skipWs();
            if (eof() || peek() != ':')
                return fail("expected : after object key");
            ++i_;

            skipWs();
            Value item;
            if (!parseValue(&item, depth + 1))
                return false;
            v.object.emplace_back(std::move(key), std::move(item));

            skipWs();
            if (eof())
                return fail("unterminated object");
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == '}') {
                ++i_;
                *out = std::move(v);
                return true;
            }
            return fail("expected , or } in object");
        }
    }

    // Reads four hex digits. Separate because a surrogate pair needs a second.
    bool hex4(unsigned* out)
    {
        if (i_ + 4 > s_.size())
            return false;
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<size_t>(k)];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        i_ += 4;
        *out = v;
        return true;
    }

    static void appendUtf8(std::string* out, unsigned cp)
    {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseString(std::string* out)
    {
        ++i_;   // opening quote
        out->clear();

        for (;;) {
            if (eof())
                return fail("unterminated string");

            const char c = s_[i_];
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c != '\\') {
                // Control characters are not legal raw inside a JSON string.
                if (static_cast<unsigned char>(c) < 0x20)
                    return fail("raw control character in string");
                out->push_back(c);
                ++i_;
                continue;
            }

            ++i_;   // backslash
            if (eof())
                return fail("unterminated escape");

            const char e = s_[i_++];
            switch (e) {
            case '"':  out->push_back('"');  break;
            case '\\': out->push_back('\\'); break;
            case '/':  out->push_back('/');  break;
            case 'b':  out->push_back('\b'); break;
            case 'f':  out->push_back('\f'); break;
            case 'n':  out->push_back('\n'); break;
            case 'r':  out->push_back('\r'); break;
            case 't':  out->push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(&cp))
                    return fail("bad \\u escape");

                // A high surrogate is only half a character. Its low partner
                // must follow, or the text is not valid UTF-16 to begin with.
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        if (!hex4(&lo))
                            return fail("bad \\u escape in surrogate pair");
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            return fail("unpaired high surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        return fail("unpaired high surrogate");
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired low surrogate");
                }

                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("unknown escape");
            }
        }
    }

    bool parseNumber(Value* out)
    {
        const size_t start = i_;

        if (!eof() && peek() == '-')
            ++i_;

        if (eof() || peek() < '0' || peek() > '9')
            return fail("expected a number");

        // Leading zeroes are not allowed by JSON, and accepting them here would
        // make this parser disagree with every other one about "007".
        if (peek() == '0') {
            ++i_;
        } else {
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i_;
        }

        bool isInt = true;

        if (!eof() && peek() == '.') {
            isInt = false;
            ++i_;
            if (eof() || peek() < '0' || peek() > '9')
                return fail("expected a digit after the decimal point");
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i_;
        }

        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            isInt = false;
            ++i_;
            if (!eof() && (peek() == '+' || peek() == '-'))
                ++i_;
            if (eof() || peek() < '0' || peek() > '9')
                return fail("expected a digit in the exponent");
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i_;
        }

        // The span above is a complete, validated JSON number. Keeping it is
        // what lets the dump side hand back the literal a person wrote instead
        // of whatever survives the trip through double.
        std::string text(s_.substr(start, i_ - start));

        if (isInt) {
            errno = 0;
            char*           end = nullptr;
            const long long n   = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                *out     = makeInt(static_cast<std::int64_t>(n));
                out->raw = std::move(text);
                return true;
            }
            // Too big for an integer. Still a valid JSON number, so it falls
            // through to a double rather than being rejected.
        }

        // strtod saturates 1e400 to infinity and rounds 0.1 to the nearest
        // representable double. Both losses stop at the parsed value: raw is
        // what gets written back out.
        *out     = makeNumber(std::strtod(text.c_str(), nullptr));
        out->raw = std::move(text);
        return true;
    }

    std::string_view s_;
    size_t           i_ = 0;
    std::string*     error_;
};

void dumpString(std::string_view s, std::string* out)
{
    out->push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"':  out->append("\\\"");  break;
        case '\\': out->append("\\\\");  break;
        case '\b': out->append("\\b");   break;
        case '\f': out->append("\\f");   break;
        case '\n': out->append("\\n");   break;
        case '\r': out->append("\\r");   break;
        case '\t': out->append("\\t");   break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out->append(buf);
            } else {
                // Anything above 0x1F is passed through, which keeps valid
                // UTF-8 intact. Escaping per byte would corrupt it.
                out->push_back(c);
            }
            break;
        }
    }
    out->push_back('"');
}

void dumpInto(const Value& v, std::string* out)
{
    switch (v.type) {
    case Value::Type::Null:
        out->append("null");
        return;
    case Value::Type::Bool:
        out->append(v.boolean ? "true" : "false");
        return;
    case Value::Type::Number:
        if (!v.raw.empty()) {
            // Parsed numbers go back out as they came in. Re-formatting turns a
            // hand-written 0.1 into 0.10000000000000001, 1.5e3 into 1500,
            // 10000000000000000000 into 1e+19 and 1e400 into null, all of them
            // during an install that was only meant to add a hook entry.
            out->append(v.raw);
        } else if (v.isInteger) {
            out->append(std::to_string(v.integer));
        } else if (!std::isfinite(v.number)) {
            // JSON has no way to write these, and emitting a bare NaN would
            // produce a document no client can parse.
            out->append("null");
        } else {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", v.number);
            out->append(buf);
        }
        return;
    case Value::Type::String:
        dumpString(v.string, out);
        return;
    case Value::Type::Array: {
        out->push_back('[');
        bool first = true;
        for (const Value& item : v.array) {
            if (!first)
                out->push_back(',');
            first = false;
            dumpInto(item, out);
        }
        out->push_back(']');
        return;
    }
    case Value::Type::Object: {
        out->push_back('{');
        bool first = true;
        for (const auto& [key, item] : v.object) {
            if (!first)
                out->push_back(',');
            first = false;
            dumpString(key, out);
            out->push_back(':');
            dumpInto(item, out);
        }
        out->push_back('}');
        return;
    }
    }
}

void dumpPrettyInto(const Value& v, int indent, int depth, std::string* out)
{
    const auto pad = [&](int level) { out->append(static_cast<size_t>(level * indent), ' '); };

    switch (v.type) {
    case Value::Type::Array: {
        if (v.array.empty()) {
            out->append("[]");
            return;
        }
        out->append("[\n");
        bool first = true;
        for (const Value& item : v.array) {
            if (!first)
                out->append(",\n");
            first = false;
            pad(depth + 1);
            dumpPrettyInto(item, indent, depth + 1, out);
        }
        out->push_back('\n');
        pad(depth);
        out->push_back(']');
        return;
    }
    case Value::Type::Object: {
        if (v.object.empty()) {
            out->append("{}");
            return;
        }
        out->append("{\n");
        bool first = true;
        for (const auto& [key, item] : v.object) {
            if (!first)
                out->append(",\n");
            first = false;
            pad(depth + 1);
            dumpString(key, out);
            out->append(": ");
            dumpPrettyInto(item, indent, depth + 1, out);
        }
        out->push_back('\n');
        pad(depth);
        out->push_back('}');
        return;
    }
    default:
        // Scalars carry no layout of their own.
        dumpInto(v, out);
        return;
    }
}

} // namespace

const Value* Value::find(std::string_view key) const
{
    if (type != Type::Object)
        return nullptr;
    for (const auto& [k, v] : object) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

Value makeNull() { return Value{}; }

Value makeBool(bool b)
{
    Value v;
    v.type    = Value::Type::Bool;
    v.boolean = b;
    return v;
}

Value makeInt(std::int64_t n)
{
    Value v;
    v.type      = Value::Type::Number;
    v.integer   = n;
    v.number    = static_cast<double>(n);
    v.isInteger = true;
    return v;
}

Value makeNumber(double d)
{
    Value v;
    v.type   = Value::Type::Number;
    v.number = d;
    return v;
}

Value makeString(std::string s)
{
    Value v;
    v.type   = Value::Type::String;
    v.string = std::move(s);
    return v;
}

Value makeArray(std::vector<Value> items)
{
    Value v;
    v.type  = Value::Type::Array;
    v.array = std::move(items);
    return v;
}

Value makeObject(std::vector<std::pair<std::string, Value>> members)
{
    Value v;
    v.type   = Value::Type::Object;
    v.object = std::move(members);
    return v;
}

bool parse(std::string_view text, Value* out, std::string* error)
{
    Parser p(text, error);
    return p.run(out);
}

std::string dump(const Value& v)
{
    std::string out;
    out.reserve(256);
    dumpInto(v, &out);
    return out;
}

std::string dumpPretty(const Value& v, int indent)
{
    std::string out;
    out.reserve(1024);
    dumpPrettyInto(v, indent < 0 ? 0 : indent, 0, &out);
    return out;
}

} // namespace pm::json
