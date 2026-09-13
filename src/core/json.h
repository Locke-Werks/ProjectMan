#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pm::json {

// A JSON value, held as plain members rather than a variant.
//
// std::variant over incomplete types is not portable, and a recursive value has
// to be incomplete inside itself. std::vector of an incomplete type is
// guaranteed to work, so the members are laid out flat and the unused ones stay
// empty. This wastes a few dozen bytes per node and buys a type that is
// obviously correct, which for a server parsing a handful of small messages is
// the right trade. The same reasoning covers `raw` below: a parsed number
// carries its own source text instead of the dump side trying to reconstruct
// one from a double.
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
    // The number exactly as the source text spelled it, empty for one built in
    // code. Neither double nor int64 can hold every JSON number: 0.1 has no
    // exact binary form, 1e400 is infinity, and 10000000000000000000 overflows
    // an int64 and comes back out as 1e+19. Re-formatting from the parsed value
    // therefore rewrites literals, and a hook install that reads a
    // hand-maintained settings file, adds one entry and writes it back must not
    // edit numbers it was never asked to touch. The parser only fills this in
    // from a span it has already validated as a JSON number, so emitting it
    // verbatim cannot produce a document that will not parse. It wins over
    // `number` and `integer` on the way out, so changing either of those on a
    // parsed value means clearing this as well.
    std::string raw;

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

// Indented, one member per line, no trailing newline.
//
// For files a person reads and edits. Rewriting a hand-maintained settings file
// through dump() would collapse it to a single line, which is a worse change
// than whatever was being edited.
std::string dumpPretty(const Value& v, int indent = 2);

} // namespace pm::json
