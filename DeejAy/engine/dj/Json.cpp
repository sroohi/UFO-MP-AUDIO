#include "Json.h"
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dj::json
{

namespace
{
    void writeString (std::string& out, const std::string& s)
    {
        out += '"';
        for (const char c : s)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char) c < 0x20) { char buf[8]; std::snprintf (buf, sizeof (buf), "\\u%04x", c); out += buf; }
                    else out += c;
            }
        }
        out += '"';
    }

    void write (std::string& out, const Value& value, int indent, int level)
    {
        const std::string pad (indent * (std::size_t) level, ' ');
        const std::string padIn (indent * (std::size_t) (level + 1), ' ');
        const char* nl = indent > 0 ? "\n" : "";
        if (value.isNull()) out += "null";
        else if (value.isBool()) out += value.boolean() ? "true" : "false";
        else if (value.isNumber())
        {
            const double d = value.number();
            if (std::isfinite (d) && d == std::floor (d) && std::abs (d) < 9.0e15) out += std::to_string ((long long) d);
            else { char buf[64]; std::snprintf (buf, sizeof (buf), "%.10g", d); out += buf; }
        }
        else if (value.isString()) writeString (out, value.string());
        else if (value.isArray())
        {
            const auto& a = value.array();
            if (a.empty()) { out += "[]"; return; }
            out += "["; out += nl;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                out += padIn; write (out, a[i], indent, level + 1);
                if (i + 1 < a.size()) out += ",";
                out += nl;
            }
            out += pad; out += "]";
        }
        else
        {
            const auto& o = value.object();
            if (o.empty()) { out += "{}"; return; }
            out += "{"; out += nl;
            std::size_t i = 0;
            for (const auto& [k, v] : o)
            {
                out += padIn; writeString (out, k); out += indent > 0 ? ": " : ":"; write (out, v, indent, level + 1);
                if (++i < o.size()) out += ",";
                out += nl;
            }
            out += pad; out += "}";
        }
    }

    struct Parser
    {
        const std::string& s; std::size_t i = 0; std::string err;
        explicit Parser (const std::string& text) : s (text) {}

        void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
        bool fail (const char* m) { if (err.empty()) err = std::string (m) + " at " + std::to_string (i); return false; }

        bool parseValue (Value& out)
        {
            ws();
            if (i >= s.size()) return fail ("unexpected end");
            const char c = s[i];
            if (c == '{') return parseObject (out);
            if (c == '[') return parseArray (out);
            if (c == '"') { std::string str; if (! parseString (str)) return false; out = Value (std::move (str)); return true; }
            if (s.compare (i, 4, "true") == 0) { i += 4; out = Value (true); return true; }
            if (s.compare (i, 5, "false") == 0) { i += 5; out = Value (false); return true; }
            if (s.compare (i, 4, "null") == 0) { i += 4; out = Value (nullptr); return true; }
            return parseNumber (out);
        }
        bool parseNumber (Value& out)
        {
            const char* start = s.c_str() + i;
            char* end = nullptr;
            const double d = std::strtod (start, &end);
            if (end == start) return fail ("bad number");
            i += (std::size_t) (end - start);
            out = Value (d);
            return true;
        }
        bool parseString (std::string& out)
        {
            if (s[i] != '"') return fail ("expected string");
            ++i;
            while (i < s.size())
            {
                const char c = s[i++];
                if (c == '"') return true;
                if (c == '\\')
                {
                    if (i >= s.size()) return fail ("bad escape");
                    const char e = s[i++];
                    switch (e)
                    {
                        case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                        case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break; case 't': out += '\t'; break;
                        case 'u':
                        {
                            if (i + 4 > s.size()) return fail ("bad unicode escape");
                            const unsigned cp = (unsigned) std::strtoul (s.substr (i, 4).c_str(), nullptr, 16);
                            i += 4;
                            if (cp < 0x80) out += (char) cp;
                            else if (cp < 0x800) { out += (char) (0xC0 | (cp >> 6)); out += (char) (0x80 | (cp & 0x3F)); }
                            else { out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
                            break;
                        }
                        default: return fail ("bad escape");
                    }
                }
                else out += c;
            }
            return fail ("unterminated string");
        }
        bool parseArray (Value& out)
        {
            ++i; Array a; ws();
            if (i < s.size() && s[i] == ']') { ++i; out = Value (std::move (a)); return true; }
            for (;;)
            {
                Value v; if (! parseValue (v)) return false;
                a.push_back (std::move (v)); ws();
                if (i >= s.size()) return fail ("unterminated array");
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == ']') { ++i; break; }
                return fail ("expected , or ]");
            }
            out = Value (std::move (a)); return true;
        }
        bool parseObject (Value& out)
        {
            ++i; Object o; ws();
            if (i < s.size() && s[i] == '}') { ++i; out = Value (std::move (o)); return true; }
            for (;;)
            {
                ws(); std::string key; if (! parseString (key)) return false; ws();
                if (i >= s.size() || s[i] != ':') return fail ("expected :");
                ++i;
                Value v; if (! parseValue (v)) return false;
                o[key] = std::move (v); ws();
                if (i >= s.size()) return fail ("unterminated object");
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == '}') { ++i; break; }
                return fail ("expected , or }");
            }
            out = Value (std::move (o)); return true;
        }
    };
}

std::string serialise (const Value& value, int indent)
{
    std::string out;
    write (out, value, indent, 0);
    return out;
}

bool parse (const std::string& text, Value& out, std::string* error)
{
    Parser p (text);
    const bool ok = p.parseValue (out);
    if (ok) { p.ws(); if (p.i != text.size()) { p.fail ("trailing characters"); if (error) *error = p.err; return false; } }
    if (! ok && error) *error = p.err;
    return ok;
}

} // namespace dj::json
