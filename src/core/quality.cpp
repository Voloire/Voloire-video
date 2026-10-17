#include "quality.h"
#include <algorithm>
#include <cmath>

namespace vv {

namespace {
// kbps for H.264 at fps > 30.
int tableKbps(const std::string& preset, int outHeight) {
  if (preset == "minimal") return 2000;
  if (outHeight <= 480) return preset == "high" ? 8000 : preset == "standard" ? 3000 : 1500;
  if (outHeight <= 720) return preset == "high" ? 20000 : preset == "standard" ? 7500 : 4000;
  if (outHeight <= 1080) return preset == "high" ? 40000 : preset == "standard" ? 12000 : 6000;
  if (outHeight <= 1440) return preset == "high" ? 60000 : preset == "standard" ? 20000 : 10000;
  return preset == "high" ? 100000 : preset == "standard" ? 40000 : 20000;
}
}  // namespace

int presetBitrateKbps(const std::string& preset, int outHeight, int fps, const std::string& codec) {
  double k = tableKbps(preset, outHeight);
  if (fps <= 30) k *= 0.65;
  if (codec == "hevc" || codec == "av1") k *= 0.7;
  int r = (int)std::llround(k / 100.0) * 100;
  return std::clamp(r, 300, 200000);
}

int presetHeightCap(const std::string& preset) { return preset == "minimal" ? 720 : 0; }

}  // namespace vv
