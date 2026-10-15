#pragma once
#include <memory>
#include <string>
#include <vector>
#include "sources.h"

namespace vv {

struct EncoderParams {
  std::string path;
  std::string container = "mkv";
  std::string codec = "h264";
  std::string encoder = "auto";
  int width = 0, height = 0;  // output size, even
  int fps = 60;
  int bitrateKbps = 12000;
  std::vector<std::string> audioTracks;  // names, e.g. {"SYS", "MIC"}
};

// FFmpeg muxer: one video stream plus one AAC stream per named track. Thread-safe for one
// video writer thread and one audio writer thread.
class Muxer {
 public:
  Muxer();
  ~Muxer();
  bool open(const EncoderParams& p, std::string& err);
  // ptsUs is the position on the recording timeline, pauses already removed.
  bool writeVideo(const VideoFrame& f, int64_t ptsUs);
  bool writeAudio(int track, const float* interleaved, size_t frames);
  bool close(std::string& err);

  std::string videoEncoderName() const;
  uint64_t videoFramesWritten() const;
  uint64_t bytesWritten() const;
  int width() const;
  int height() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace vv
