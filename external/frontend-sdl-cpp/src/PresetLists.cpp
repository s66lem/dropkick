#include "PresetLists.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace PresetLists
{
namespace
{
struct SettingRange
{
    std::string_view key;
    double minimum;
    double maximum;
};

constexpr SettingRange settings[]{
    {"presetDuration", 1, 3600}, {"softCutDuration", 0, 30}, {"hardCut", 0, 1},
    {"hardCutDuration", 0, 300}, {"hardCutSensitivity", 0, 10}, {"beatSensitivity", 0, 5},
    {"fps", 10, 240}, {"aspectCorrection", 0, 1}, {"reduceFlashing", 0, 1},
    {"flashStrength", 0, 1}, {"brightness", 0.05, 1}, {"tintEnabled", 0, 1},
    {"tintColor", 0, 0}, {"tintStrength", 0, 1}, {"autoskipEnabled", 0, 1},
    {"autoskipFps", 1, 120}, {"autoskipStrikes", 0, 50}, {"livePreview", 0, 1}
};

const SettingRange* FindSetting(std::string_view key)
{
    for (const auto& setting : settings)
    {
        if (setting.key == key) { return &setting; }
    }
    return nullptr;
}

bool EnsureParentDirectory(const fs::path& path)
{
    if (!path.has_parent_path()) { return true; }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return !ec;
}
}

bool EncodeBmp24FromRgba(int width, int height, const uint8_t* rgbaBottomUp, std::size_t rgbaSize,
                         std::vector<uint8_t>& outBmp)
{
    if (width <= 0 || height <= 0 || !rgbaBottomUp) { return false; }

    constexpr uint32_t headerSize = 54;
    const auto bmpRowBytes = static_cast<uint64_t>(width) * 3;
    const auto stride = (bmpRowBytes + 3) & ~uint64_t{3};
    const auto rows = static_cast<uint64_t>(height);
    const auto rgbaStride = static_cast<uint64_t>(width) * 4;
    const auto maxBmpSize = std::min<uint64_t>(UINT32_MAX, outBmp.max_size());
    if (rgbaStride > rgbaSize / rows || maxBmpSize < headerSize ||
        stride > (maxBmpSize - headerSize) / rows)
    {
        return false;
    }

    const auto imageSize = static_cast<uint32_t>(stride * rows);
    const auto fileSize = headerSize + imageSize;
    std::vector<uint8_t> bmp(fileSize, 0);
    const auto writeLe = [&bmp](std::size_t offset, uint32_t value, unsigned int bytes) {
        for (unsigned int i = 0; i < bytes; ++i)
        {
            bmp[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    writeLe(2, fileSize, 4);
    writeLe(10, headerSize, 4);
    writeLe(14, 40, 4);
    writeLe(18, static_cast<uint32_t>(width), 4);
    writeLe(22, static_cast<uint32_t>(height), 4);
    writeLe(26, 1, 2);
    writeLe(28, 24, 2);
    writeLe(34, imageSize, 4);

    for (uint64_t y = 0; y < rows; ++y)
    {
        const auto* rgba = rgbaBottomUp + y * rgbaStride;
        auto* bgr = bmp.data() + headerSize + y * stride;
        for (int x = 0; x < width; ++x, rgba += 4, bgr += 3)
        {
            bgr[0] = rgba[2];
            bgr[1] = rgba[1];
            bgr[2] = rgba[0];
        }
    }
    outBmp.swap(bmp);
    return true;
}

fs::path PathFromUtf8(std::string_view path)
{
    if (path.empty() || path.find('\0') != path.npos) { return {}; }
#ifdef _WIN32
    if (path.size() > static_cast<size_t>(std::numeric_limits<int>::max())) { return {}; }
    const int size = static_cast<int>(path.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), size, nullptr, 0);
    if (length == 0) { return {}; }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), size, wide.data(), length) == 0) { return {}; }
    return fs::path(wide);
#else
    return fs::path(path);
#endif
}

std::string PathToUtf8(const fs::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string NormalizePathSeparators(std::string path)
{
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    return path;
}

bool ConstantTimeEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) { return false; }
    // Keep the reduction observable so it can't become an early-exit comparison.
    volatile unsigned int diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        diff = diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

bool ParseHexColor(std::string_view hex, float& r, float& g, float& b)
{
    if (!hex.empty() && hex.front() == '#') { hex.remove_prefix(1); }
    if (hex.size() != 6) { return false; }
    uint32_t rgb;
    const auto end = hex.data() + hex.size();
    const auto result = std::from_chars(hex.data(), end, rgb, 16);
    if (result.ec != std::errc{} || result.ptr != end) { return false; }
    r = static_cast<float>(rgb >> 16) / 255.0f;
    g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    b = static_cast<float>(rgb & 0xFF) / 255.0f;
    return true;
}

bool WriteFileAtomic(const std::string& path, std::string_view contents)
{
    return WriteFileAtomic(PathFromUtf8(path), contents, true) == WriteFileResult::Written;
}

WriteFileResult WriteFileAtomic(const fs::path& target, std::string_view contents, bool overwrite)
{
    if (target.empty()) { return WriteFileResult::Failed; }
    std::error_code ec;
    if (!overwrite && fs::exists(fs::symlink_status(target, ec))) { return WriteFileResult::Exists; }
    if (!EnsureParentDirectory(target)) { return WriteFileResult::Failed; }

    static std::atomic<uint64_t> nextTemp{0};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    fs::path temp = target;
    temp += ".tmp." + std::to_string(pid) + "." + std::to_string(nextTemp.fetch_add(1, std::memory_order_relaxed));

#ifdef _WIN32
    std::FILE* out = nullptr;
    if (_wfopen_s(&out, temp.c_str(), L"wb") != 0) { return WriteFileResult::Failed; }
#else
    std::FILE* out = std::fopen(temp.c_str(), "wb");
    if (!out) { return WriteFileResult::Failed; }
#endif
    bool written = (contents.empty() || std::fwrite(contents.data(), 1, contents.size(), out) == contents.size())
                   && std::fflush(out) == 0;
#ifdef _WIN32
    written = written && _commit(_fileno(out)) == 0;
#else
    written = written && fsync(fileno(out)) == 0;
#endif
    written = std::fclose(out) == 0 && written;

    auto result = WriteFileResult::Failed;
    if (written)
    {
#ifdef _WIN32
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
        if (MoveFileExW(temp.c_str(), target.c_str(), flags)) { return WriteFileResult::Written; }
        const auto error = GetLastError();
        if (!overwrite && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) { result = WriteFileResult::Exists; }
#else
        if (overwrite) { fs::rename(temp, target, ec); }
        else
        {
            fs::create_hard_link(temp, target, ec);
            if (!ec) { fs::remove(temp, ec); }
            else if (ec == std::errc::file_exists) { result = WriteFileResult::Exists; }
        }
        if (!ec)
        {
            const fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
            const int dir = open(parent.c_str(), O_RDONLY);
            if (dir < 0) { return WriteFileResult::Failed; }
            const bool synced = fsync(dir) == 0;
            close(dir);
            return synced ? WriteFileResult::Written : WriteFileResult::Failed;
        }
#endif
    }
    fs::remove(temp, ec);
    return result;
}

std::unordered_set<std::string> LoadLineSet(const std::string& path)
{
    std::unordered_set<std::string> entries;
    std::ifstream in(PathFromUtf8(path));
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty()) { entries.insert(line); }
    }
    return entries;
}

bool AppendLine(const std::string& path, const std::string& line)
{
    const auto target = PathFromUtf8(path);
    if (target.empty() || !EnsureParentDirectory(target)) { return false; }
    std::ofstream out(target, std::ios::app);
    out << line << '\n';
    out.close();
    return static_cast<bool>(out);
}

std::unordered_map<std::string, uint32_t> LoadSlowCounts(const std::string& path)
{
    std::unordered_map<std::string, uint32_t> counts;
    std::ifstream in(PathFromUtf8(path));
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        const auto tab = line.find('\t');
        if (tab == line.npos || tab + 1 == line.size()) { continue; }
        uint32_t count;
        const char* end = line.data() + tab;
        const auto result = std::from_chars(line.data(), end, count);
        if (result.ec == std::errc{} && result.ptr == end && count > 0)
        {
            counts[line.substr(tab + 1)] = count;
        }
    }
    return counts;
}

bool SaveSlowCountsAtomic(const std::string& path, const std::unordered_map<std::string, uint32_t>& counts)
{
    std::string text;
    for (const auto& entry : counts)
    {
        text += std::to_string(entry.second);
        text += '\t';
        text += entry.first;
        text += '\n';
    }
    return WriteFileAtomic(path, text);
}

bool IsValidSettingKey(std::string_view key)
{
    return FindSetting(key) != nullptr;
}

bool IsValidWorkshopFilename(std::string_view name)
{
    return !name.empty() && name.find("..") == name.npos &&
           name.find_first_of("/\\:") == name.npos && name.find('\0') == name.npos;
}

double ClampSettingValue(std::string_view key, double v)
{
    const auto* setting = FindSetting(key);
    if (!setting || key == "tintColor") { return v; }
    return std::isnan(v) ? setting->minimum : std::clamp(v, setting->minimum, setting->maximum);
}
}
