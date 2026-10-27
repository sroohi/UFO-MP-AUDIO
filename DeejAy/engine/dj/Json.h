// Minimal JSON value + parser/serialiser for the library database, mappings and settings. Message thread only.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <cstdint>

namespace dj::json
{

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value
{
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v;

    Value() : v (nullptr) {}
    Value (std::nullptr_t) : v (nullptr) {}
    Value (bool b) : v (b) {}
    Value (int i) : v ((double) i) {}
    Value (std::int64_t i) : v ((double) i) {}
    Value (double d) : v (d) {}
    Value (const char* s) : v (std::string (s)) {}
    Value (std::string s) : v (std::move (s)) {}
    Value (Array a) : v (std::move (a)) {}
    Value (Object o) : v (std::move (o)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t> (v); }
    bool isObject() const { return std::holds_alternative<Object> (v); }
    bool isArray() const { return std::holds_alternative<Array> (v); }
    bool isString() const { return std::holds_alternative<std::string> (v); }
    bool isNumber() const { return std::holds_alternative<double> (v); }
    bool isBool() const { return std::holds_alternative<bool> (v); }

    double number (double def = 0.0) const { return isNumber() ? std::get<double> (v) : def; }
    std::int64_t integer (std::int64_t def = 0) const { return isNumber() ? (std::int64_t) std::get<double> (v) : def; }
    bool boolean (bool def = false) const { return isBool() ? std::get<bool> (v) : def; }
    const std::string& string() const { static const std::string empty; return isString() ? std::get<std::string> (v) : empty; }
    const Array& array() const { static const Array empty; return isArray() ? std::get<Array> (v) : empty; }
    const Object& object() const { static const Object empty; return isObject() ? std::get<Object> (v) : empty; }

    const Value& operator[] (const std::string& key) const
    {
        static const Value nullValue;
        if (! isObject()) return nullValue;
        const auto& o = std::get<Object> (v);
        const auto it = o.find (key);
        return it == o.end() ? nullValue : it->second;
    }
    Value& set (const std::string& key, Value val)
    {
        if (! isObject()) v = Object {};
        std::get<Object> (v)[key] = std::move (val);
        return *this;
    }
};

std::string serialise (const Value& value, int indent = 0);
bool parse (const std::string& text, Value& out, std::string* error = nullptr);

} // namespace dj::json
