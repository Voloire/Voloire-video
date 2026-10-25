#pragma once
#include <algorithm>
#include <cmath>

namespace vv {

struct VideoSize { int width, height; };
struct VideoRect { int x, y, width, height; };

// Named streaming resolutions are exact 16:9 canvases; zero explicitly keeps native size.
inline VideoSize streamCanvas(int sourceWidth, int sourceHeight, int height) {
  if (height <= 0) return {std::max(2, sourceWidth & ~1), std::max(2, sourceHeight & ~1)};
  return {std::max(2, (int)std::lround(height * 8.0 / 9.0) * 2), std::max(2, height & ~1)};
}

// NV12 content and offsets must be even. Asymmetric padding can differ by two pixels.
inline VideoRect fitVideo(int sourceWidth, int sourceHeight, int width, int height) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || width < 2 || height < 2) return {0, 0, 0, 0};
  double scale = std::min((double)width / sourceWidth, (double)height / sourceHeight);
  int w = std::min(width & ~1, std::max(2, (int)std::floor(sourceWidth * scale + 1e-6) & ~1));
  int h = std::min(height & ~1, std::max(2, (int)std::floor(sourceHeight * scale + 1e-6) & ~1));
  return {((width - w) / 2) & ~1, ((height - h) / 2) & ~1, w, h};
}

}  // namespace vv
