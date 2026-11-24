#include "JsonUtil.h"

#include <charconv>
#include <cstdint>
#include <utility>

namespace JsonUtil
{
namespace
{
bool Consume(std::string_view& text, char expected)
{
    const auto pos = text.find_first_not_of(" \t\r\n");
    if (pos == text.npos || text[pos] != expected) { return false; }
    text.remove_prefix(pos + 1);
    return true;
}

bool ReadCodeUnit(std::string_view& text, uint32_t& code)
{
    if (text.size() < 4) { return false; }
    const auto end = text.data() + 4;
    const auto result = std::from_chars(text.data(), end, code, 16);
    if (result.ec != std::errc{} || result.ptr != end) { return false; }
    text.remove_prefix(4);
    return true;
}

bool ReadUnicode(std::string_view& text, std::string& out)
{
    uint32_t code;
    if (!ReadCodeUnit(text, code)) { return false; }
    if (code >= 0xD800 && code <= 0xDBFF)
    {
        if (text.substr(0, 2) != "\\u") { return false; }
        text.remove_prefix(2);
        uint32_t low;
        if (!ReadCodeUnit(text, low) || low < 0xDC00 || low > 0xDFFF) { return false; }
        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
    }
    else if (code >= 0xDC00 && code <= 0xDFFF)
    {
        return false;
    }
    if (code >= 0x10000)
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    }
    else if (code >= 0x800)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
    }
    else if (code >= 0x80)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
    }
    if (code >= 0x800) { out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); }
    out += static_cast<char>(code < 0x80 ? code : (0x80 | (code & 0x3F)));
    return true;
}

bool ReadString(std::string_view& text, std::string& out)
{
    while (!text.empty())
    {
        const char c = text.front();
        text.remove_prefix(1);
        if (c == '"') { return true; }
        if (static_cast<unsigned char>(c) < 0x20) { return false; }
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (text.empty()) { return false; }
        const char escape = text.front();
        text.remove_prefix(1);
        switch (escape)
        {
            case '"': case '\\': case '/': out += escape; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': if (!ReadUnicode(text, out)) { return false; } break;
            default: return false;
        }
    }
    return false;
}
}

std::string EscapeJson(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    AppendJsonEscaped(out, in);
    return out;
}

void AppendJsonEscaped(std::string& out, std::string_view in)
{
    for (char c : in)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20)
                {
                    constexpr char hex[] = "0123456789ABCDEF";
                    out += "\\u00";
                    out += hex[byte >> 4];
                    out += hex[byte & 0xF];
                }
                else { out += c; }
                break;
        }
    }
}

std::vector<std::string> ParseJsonStringArray(std::string_view text)
{
    std::vector<std::string> out;
    if (!Consume(text, '[') || Consume(text, ']')) { return out; }
    do
    {
        std::string value;
        if (!Consume(text, '"') || !ReadString(text, value)) { return {}; }
        out.push_back(std::move(value));
    } while (Consume(text, ','));
    if (!Consume(text, ']') || text.find_first_not_of(" \t\r\n") != text.npos) { return {}; }
    return out;
}
}
