#pragma once
#include <atomic>
#include <functional>
#include <string>
#include "common.h"

namespace vv {

struct SharePlan {
  int width = 0, height = 0, fps = 0, videoKbps = 0, audioKbps = 0;
  std::string error;
};

// Pure. Fits `durationSec` of `srcW x srcH @ srcFps` into `targetMB` (1 MB = 1 MiB): picks the
// video bitrate, then lowers fps and output size until the bits per pixel per second are sane.
// When even the minimum bitrate does not fit, error names the longest duration that would.
SharePlan planShare(double durationSec, int srcW, int srcH, double srcFps, int targetMB, bool hasAudio);

struct ShareResult {
  std::string path, error;
  uint64_t bytes = 0;
  int attempts = 0;
  json toJson() const;
};

// Decode `src` and re-encode it at planShare's settings into `<src stem>_share_<MB>MB.mp4`
// beside it, CBR and faststart. progress() receives 0..1; cancel (may be null) aborts.
ShareResult makeShareCopy(const std::string& src, int targetMB, std::function<void(double)> progress = nullptr,
                          std::atomic<bool>* cancel = nullptr);

}  // namespace vv
