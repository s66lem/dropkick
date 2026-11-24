#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace PresetLists
{
enum class WriteFileResult { Written, Exists, Failed };

std::filesystem::path PathFromUtf8(std::string_view path);
std::string PathToUtf8(const std::filesystem::path& path);
std::string NormalizePathSeparators(std::string path);
bool ConstantTimeEquals(std::string_view a, std::string_view b);
bool ParseHexColor(std::string_view hex, float& r, float& g, float& b);
bool WriteFileAtomic(const std::string& path, std::string_view contents);
WriteFileResult WriteFileAtomic(const std::filesystem::path& path, std::string_view contents, bool overwrite);
std::unordered_set<std::string> LoadLineSet(const std::string& path);
bool AppendLine(const std::string& path, const std::string& line);
std::unordered_map<std::string, uint32_t> LoadSlowCounts(const std::string& path);
bool SaveSlowCountsAtomic(const std::string& path, const std::unordered_map<std::string, uint32_t>& counts);
bool IsValidWorkshopFilename(std::string_view name);
bool IsValidSettingKey(std::string_view key);
double ClampSettingValue(std::string_view key, double v);
bool EncodeBmp24FromRgba(int width, int height, const uint8_t* rgbaBottomUp, std::size_t rgbaSize,
                         std::vector<uint8_t>& outBmp);
}
