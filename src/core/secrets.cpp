#include "secrets.h"
#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include "config.h"

namespace vv {

namespace fs = std::filesystem;

namespace {
fs::path secretPath(const std::string& name) {
  return pathOf(settingsPath()).parent_path() / "secrets" / pathOf(name + ".bin");
}
}  // namespace

bool saveSecret(const std::string& name, const std::string& value) {
  try {
    auto p = secretPath(name);
    fs::create_directories(p.parent_path());
    DATA_BLOB in{(DWORD)value.size(), (BYTE*)value.data()}, out{};
    if (!CryptProtectData(&in, L"voloire-video", nullptr, nullptr, nullptr, 0, &out)) return false;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return f.good();
  } catch (...) {
    return false;
  }
}

std::string loadSecret(const std::string& name) {
  try {
    std::ifstream f(secretPath(name), std::ios::binary);
    if (!f) return {};
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.empty()) return {};
    DATA_BLOB in{(DWORD)raw.size(), (BYTE*)raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return {};
    std::string v((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return v;
  } catch (...) {
    return {};
  }
}

void deleteSecret(const std::string& name) {
  try {
    fs::remove(secretPath(name));
  } catch (...) {
  }
}

}  // namespace vv
