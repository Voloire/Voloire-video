#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include "json.hpp"

namespace vv {

using json = nlohmann::json;

// Monotonic microseconds from QueryPerformanceCounter. The single clock of the whole app.
int64_t nowUs();

std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);

// UTF-8 <-> filesystem::path without the char8_t detour of C++20.
inline std::filesystem::path pathOf(const std::string& s) { return std::filesystem::path(wide(s)); }
inline std::string strOf(const std::filesystem::path& p) { return utf8(p.wstring()); }

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

}  // namespace vv
