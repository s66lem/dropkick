#include "JsonUtil.h"
#include "PresetLists.h"
#include "SystemMonitor.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace
{
unsigned int checks = 0;
unsigned int failures = 0;

void Check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "Line " << line << ": " << expression << '\n';
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

class TempDirectory
{
public:
    TempDirectory()
    {
        const auto parent = fs::temp_directory_path();
        std::random_device id;
        do
        {
            path = parent / ("dropkick-core-" + std::to_string(id()));
        } while (!fs::create_directory(path));
    }

    ~TempDirectory()
    {
        std::error_code error;
        fs::remove_all(path, error);
        CHECK(!error);
    }

    fs::path path;
};

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void TestJson()
{
    const auto original = std::string("\"\\\b\f\n\r\t") + '\0' + '\x01' + '\x1f';
    const std::string escaped = "\\\"\\\\\\b\\f\\n\\r\\t\\u0000\\u0001\\u001F";
    CHECK(JsonUtil::EscapeJson(original) == escaped);
    CHECK(JsonUtil::EscapeJson(std::string_view("\0a", 2)) == "\\u0000a");
    CHECK(JsonUtil::EscapeJson("caf\xC3\xA9") == "caf\xC3\xA9");
    std::string json = "[\"";
    JsonUtil::AppendJsonEscaped(json, original);
    json += "\",\"\", \"folder/preset.milk\"]";
    const std::vector<std::string> expected{original, "", "folder/preset.milk"};
    CHECK(JsonUtil::ParseJsonStringArray(json) == expected);
    CHECK(JsonUtil::ParseJsonStringArray(" [ \"\\/\\u00e9\\ud83d\\ude00\" ] ") ==
          std::vector<std::string>{"/\xC3\xA9\xF0\x9F\x98\x80"});
    CHECK(JsonUtil::ParseJsonStringArray("[\"\\u001b\\u001F\"]") ==
          std::vector<std::string>{"\x1b\x1f"});
    CHECK(JsonUtil::ParseJsonStringArray("[]").empty());
    CHECK(JsonUtil::ParseJsonStringArray("[\"incomplete").empty());
    CHECK(JsonUtil::ParseJsonStringArray("[\"one\",]").empty());
    CHECK(JsonUtil::ParseJsonStringArray("[\"one\"] trailing").empty());
    CHECK(JsonUtil::ParseJsonStringArray("[\"\\ud800\"]").empty());
    CHECK(JsonUtil::ParseJsonStringArray("[\"\\uXXXX\"]").empty());
}

void TestPathsAndAuth()
{
#ifdef _WIN32
    CHECK(PresetLists::NormalizePathSeparators("root\\pack/a.milk") == "root/pack/a.milk");
#else
    CHECK(PresetLists::NormalizePathSeparators("root\\pack/a.milk") == "root\\pack/a.milk");
#endif
    CHECK(PresetLists::ConstantTimeEquals("", ""));
    CHECK(PresetLists::ConstantTimeEquals("secret", "secret"));
    CHECK(!PresetLists::ConstantTimeEquals("secret", "secret!"));
    CHECK(!PresetLists::ConstantTimeEquals("secret", "Secret"));
    CHECK(!PresetLists::ConstantTimeEquals("secret", "secreT"));
    CHECK(!PresetLists::ConstantTimeEquals(std::string_view("s\0a", 3), std::string_view("s\0b", 3)));
    CHECK(PresetLists::IsValidWorkshopFilename("caf\xC3\xA9.milk"));
    CHECK(!PresetLists::IsValidWorkshopFilename("ab:c"));
    CHECK(!PresetLists::IsValidWorkshopFilename("C:"));
    CHECK(!PresetLists::IsValidWorkshopFilename("../test.milk"));
    CHECK(!PresetLists::IsValidWorkshopFilename(std::string_view("one\0two", 7)));
}

void TestColorsAndSettings()
{
    float r = -1.0f, g = -1.0f, b = -1.0f;
    CHECK(PresetLists::ParseHexColor("#ff8000", r, g, b));
    CHECK(r == 1.0f && std::abs(g - 128.0f / 255.0f) < 0.000001f && b == 0.0f);
    CHECK(PresetLists::ParseHexColor("00AbFF", r, g, b));
    CHECK(r == 0.0f && std::abs(g - 171.0f / 255.0f) < 0.000001f && b == 1.0f);
    CHECK(!PresetLists::ParseHexColor("#fff", r, g, b));
    CHECK(!PresetLists::ParseHexColor("12345g", r, g, b));
    CHECK(!PresetLists::ParseHexColor("0000000", r, g, b));
    CHECK(!PresetLists::ParseHexColor(" 00000", r, g, b));
    CHECK(r == 0.0f && b == 1.0f);

    const struct { const char* key; double low; double high; } settings[]{
        {"presetDuration", 1, 3600}, {"softCutDuration", 0, 30}, {"hardCutDuration", 0, 300},
        {"hardCutSensitivity", 0, 10}, {"beatSensitivity", 0, 5}, {"fps", 10, 240},
        {"flashStrength", 0, 1}, {"brightness", 0.05, 1}, {"tintStrength", 0, 1},
        {"autoskipFps", 1, 120}, {"autoskipStrikes", 0, 50}, {"livePreview", 0, 1}
    };
    for (const auto& setting : settings)
    {
        const double mid = (setting.low + setting.high) / 2.0;
        CHECK(PresetLists::IsValidSettingKey(setting.key));
        CHECK(PresetLists::ClampSettingValue(setting.key, setting.low - 1.0) == setting.low);
        CHECK(PresetLists::ClampSettingValue(setting.key, setting.high + 1.0) == setting.high);
        CHECK(PresetLists::ClampSettingValue(setting.key, mid) == mid);
    }
    for (const char* key : {"hardCut", "aspectCorrection", "reduceFlashing", "tintEnabled", "tintColor", "autoskipEnabled"})
    {
        CHECK(PresetLists::IsValidSettingKey(key));
    }
    CHECK(!PresetLists::IsValidSettingKey("projectM.fps"));
    CHECK(!PresetLists::IsValidSettingKey(""));
    CHECK(PresetLists::ClampSettingValue("brightness", std::numeric_limits<double>::quiet_NaN()) == 0.05);
    CHECK(PresetLists::ClampSettingValue("brightness", 0.0) == 0.05);
    CHECK(PresetLists::ClampSettingValue("brightness", -std::numeric_limits<double>::infinity()) == 0.05);
    CHECK(PresetLists::ClampSettingValue("brightness", std::numeric_limits<double>::infinity()) == 1.0);
    CHECK(PresetLists::ClampSettingValue("tintColor", 0xFFAA00) == 0xFFAA00);
    CHECK(PresetLists::ClampSettingValue("unknown", -2.0) == -2.0);
}

void TestBmp()
{
    std::vector<uint8_t> bmp;
    const auto readLe = [&bmp](std::size_t offset, unsigned int bytes) {
        uint32_t value = 0;
        for (unsigned int i = 0; i < bytes; ++i)
        {
            value |= static_cast<uint32_t>(bmp.at(offset + i)) << (8 * i);
        }
        return value;
    };

    const std::vector<uint8_t> preview(240 * 150 * 4, 255);
    CHECK(PresetLists::EncodeBmp24FromRgba(240, 150, preview.data(), preview.size(), bmp));
    CHECK(bmp.size() == 108054);
    CHECK(bmp.at(0) == 'B' && bmp.at(1) == 'M');
    CHECK(readLe(2, 4) == bmp.size() && readLe(6, 4) == 0);
    CHECK(readLe(10, 4) == 54 && readLe(14, 4) == 40);
    CHECK(readLe(18, 4) == 240 && readLe(22, 4) == 150);
    CHECK(readLe(26, 2) == 1 && readLe(28, 2) == 24);
    CHECK(readLe(30, 4) == 0 && readLe(34, 4) == 108000);
    CHECK(bmp.at(54) == 255 && bmp.back() == 255);

    const uint8_t rgba[]{
        1, 2, 3, 100, 4, 5, 6, 101, 7, 8, 9, 102,
        10, 11, 12, 103, 13, 14, 15, 104, 16, 17, 18, 105
    };
    const std::vector<uint8_t> expected{
        3, 2, 1, 6, 5, 4, 9, 8, 7, 0, 0, 0,
        12, 11, 10, 15, 14, 13, 18, 17, 16, 0, 0, 0
    };
    CHECK(PresetLists::EncodeBmp24FromRgba(3, 2, rgba, sizeof(rgba), bmp));
    CHECK(bmp.size() == 78 && readLe(34, 4) == 24);
    CHECK(readLe(18, 4) == 3 && readLe(22, 4) == 2);
    CHECK(std::vector<uint8_t>(bmp.begin() + 54, bmp.end()) == expected);

    CHECK(!PresetLists::EncodeBmp24FromRgba(0, 2, rgba, sizeof(rgba), bmp));
    CHECK(!PresetLists::EncodeBmp24FromRgba(3, -1, rgba, sizeof(rgba), bmp));
    CHECK(!PresetLists::EncodeBmp24FromRgba(3, 2, nullptr, sizeof(rgba), bmp));
    CHECK(!PresetLists::EncodeBmp24FromRgba(3, 2, rgba, sizeof(rgba) - 1, bmp));
    CHECK(!PresetLists::EncodeBmp24FromRgba(1, std::numeric_limits<int>::max(), rgba,
                                           std::numeric_limits<std::size_t>::max(), bmp));
    CHECK(bmp.size() == 78);
}

void TestFiles()
{
    TempDirectory temp;
    using PresetLists::PathToUtf8;
    using PresetLists::WriteFileResult;
    CHECK(!PresetLists::WriteFileAtomic("", "missing destination"));
    const auto favorites = temp.path / "nested" / "favorites.json";
    const auto favoritesPath = PathToUtf8(favorites);
    CHECK(PresetLists::WriteFileAtomic(favoritesPath, "[\"first\"]"));
    CHECK(ReadFile(favorites) == "[\"first\"]");
    CHECK(PresetLists::WriteFileAtomic(favorites, "rejected", false) == WriteFileResult::Exists);
    CHECK(ReadFile(favorites) == "[\"first\"]");
    CHECK(PresetLists::WriteFileAtomic(favoritesPath, "[]"));
    CHECK(ReadFile(favorites) == "[]");
    CHECK(!PresetLists::WriteFileAtomic(PathToUtf8(favorites / "child"), "blocked"));
    CHECK(!PresetLists::WriteFileAtomic(PathToUtf8(favorites.parent_path()), "cannot replace a directory"));
    CHECK(std::distance(fs::directory_iterator(favorites.parent_path()), fs::directory_iterator{}) == 1);
    CHECK(ReadFile(favorites) == "[]");
    CHECK(PresetLists::WriteFileAtomic(favoritesPath, {}));
    CHECK(fs::file_size(favorites) == 0);

    const auto newFile = temp.path / "new.milk";
    CHECK(PresetLists::WriteFileAtomic(newFile, "created", false) == WriteFileResult::Written);
    CHECK(ReadFile(newFile) == "created");

    const auto list = PathToUtf8(temp.path / "lists" / "blocklist.txt");
    CHECK(PresetLists::LoadLineSet(list).empty());
    CHECK(PresetLists::AppendLine(list, "first.milk"));
    CHECK(PresetLists::AppendLine(list, "second.milk\r"));
    CHECK(PresetLists::AppendLine(list, "first.milk"));
    CHECK(PresetLists::AppendLine(list, ""));
    const std::unordered_set<std::string> expectedPaths{"first.milk", "second.milk"};
    CHECK(PresetLists::LoadLineSet(list) == expectedPaths);

    const auto slow = PathToUtf8(temp.path / "slowcounts.txt");
    CHECK(PresetLists::LoadSlowCounts(slow).empty());
    const std::unordered_map<std::string, uint32_t> counts{{"a.milk", 1}, {"folder/b\tname.milk", UINT32_MAX}};
    CHECK(PresetLists::SaveSlowCountsAtomic(slow, counts));
    CHECK(PresetLists::LoadSlowCounts(slow) == counts);
    CHECK(PresetLists::AppendLine(slow, "0\tzero"));
    CHECK(PresetLists::AppendLine(slow, "-1\tnegative"));
    CHECK(PresetLists::AppendLine(slow, "4294967296\toverflow"));
    CHECK(PresetLists::AppendLine(slow, "2bad\tmalformed"));
    CHECK(PresetLists::AppendLine(slow, "3\t"));
    CHECK(PresetLists::LoadSlowCounts(slow) == counts);
    CHECK(PresetLists::SaveSlowCountsAtomic(slow, {}));
    CHECK(PresetLists::LoadSlowCounts(slow).empty());

#ifdef _WIN32
    const auto utf8Dir = PathToUtf8(temp.path) + "/caf\xC3\xA9/";
    const auto utf8List = utf8Dir + "\xE9\x9B\xAA.txt";
    CHECK(PresetLists::WriteFileAtomic(utf8List, "first.milk\n"));
    CHECK(ReadFile(temp.path / L"caf\u00E9" / L"\u96EA.txt") == "first.milk\n");
    CHECK(PresetLists::AppendLine(utf8List, "second.milk"));
    CHECK(PresetLists::LoadLineSet(utf8List) == expectedPaths);
    const auto utf8Slow = utf8Dir + "slow-\xE9\x9B\xAA.txt";
    CHECK(PresetLists::SaveSlowCountsAtomic(utf8Slow, counts));
    CHECK(PresetLists::LoadSlowCounts(utf8Slow) == counts);
#endif
}

void TestMonitor()
{
    SystemMonitor monitor;
    CHECK(monitor.Sample().cpuPct == 0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto stats = monitor.Sample();
    CHECK(stats.cpuPct >= 0.0f && stats.cpuPct <= 100.0f);
    CHECK(stats.memUsedMB >= 0 && stats.memUsedMB <= stats.memTotalMB);
#if defined(_WIN32) || defined(__linux__)
    CHECK(stats.memTotalMB > 0);
#endif
#ifdef _WIN32
    CHECK(!stats.hasTemp && stats.tempC == 0.0f);
#elif defined(__linux__)
    CHECK(!stats.hasTemp || std::isfinite(stats.tempC));
#endif
}
}

int main()
{
    try
    {
        TestJson();
        TestPathsAndAuth();
        TestColorsAndSettings();
        TestBmp();
        TestFiles();
        TestMonitor();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << checks - failures << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
