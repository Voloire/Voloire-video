#pragma once
#include <string>

namespace vv {

// Recording quality presets: a table of bitrates by output height, scaled by frame rate and
// codec. "custom" is not a preset: the caller keeps the bitrate it was given.
int presetBitrateKbps(const std::string& preset, int outHeight, int fps, const std::string& codec);

// Height cap of the preset: 720 for "minimal", 0 (no cap) otherwise.
int presetHeightCap(const std::string& preset);

}  // namespace vv
