#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace JsonUtil
{
std::string EscapeJson(std::string_view in);
void AppendJsonEscaped(std::string& out, std::string_view in);
std::vector<std::string> ParseJsonStringArray(std::string_view text);
}
