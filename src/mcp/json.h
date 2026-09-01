#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pm::mcp::json {

// A JSON value, held as plain members rather than a variant.
//
// std::variant over incomplete types is not portable, and a recursive value has
// to be incomplete inside itself. std::vector of an incomplete type is
// guaranteed to work, so the members are laid out flat and the unused ones stay
// empty. This wastes a few dozen bytes per node and buys a type that is
// obviously correct, which for a server parsing a handful of small messages is
// the right trade.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;

    bool         boolean = false;
    double       number  = 0;
    std::int64_t integer = 0;
    // JSON has one number type; JSON-RPC ids are usually integers and must come
    // back byte-for-byte as they went in. Tracking which one it was is what
    // keeps id 1 from being answered as 1.0.
    bool isInteger = false;

    std::string                                string;
    std::vector<Value>                         array;
    std::vector<std::pair<std::string, Value>> object;

    // Object lookup. Returns nullptr when absent, so a caller can tell an
    // absent key from one explicitly set to null.
    const Value* find(std::string_view key) const;

    bool isNull() const { return type == Type::Null; }
};

Value makeNull();
Value makeBool(bool b);
Value makeInt(std::int64_t n);
Value makeNumber(double d);
Value makeString(std::string s);
Value makeArray(std::vector<Value> items);
Value makeObject(std::vector<std::pair<std::string, Value>> members);

// Strict parse of one complete document. False on anything malformed, with a
// human-readable reason. Nesting is capped: a client is not trusted to send
// something that would recurse this process to death.
bool parse(std::string_view text, Value* out, std::string* error);

// Compact, no spaces, no trailing newline.
std::string dump(const Value& v);

} // namespace pm::mcp::json
