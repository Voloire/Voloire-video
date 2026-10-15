#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common.h"
#include "config.h"

namespace vv {

struct VideoFrame {
  int w = 0, h = 0;
  std::vector<uint8_t> bgra;  // w*h*4, tightly packed
  int64_t capUs = 0;
  uint64_t seq = 0;
};
using FramePtr = std::shared_ptr<const VideoFrame>;

class IVideoSource {
 public:
  virtual ~IVideoSource() = default;
  // sink is called from a source thread for every new frame; onClosed once if the source disappears.
  virtual bool start(std::function<void(FramePtr)> sink, std::function<void(const std::string&)> onClosed,
                     std::string& err) = 0;
  virtual void stop() = 0;
  virtual std::string name() const = 0;
};

// Thread-safe FIFO of interleaved float32 stereo frames at 48 kHz.
class AudioRing {
 public:
  void push(const float* interleaved, size_t frames);
  size_t pop(float* dst, size_t maxFrames);
  size_t available() const;
  void clear();
  uint64_t droppedFrames() const { return dropped_; }

 private:
  mutable std::mutex mu_;
  std::vector<float> buf_;
  size_t head_ = 0;  // in samples
  std::atomic<uint64_t> dropped_{0};
  static constexpr size_t kMaxFrames = kSampleRate * 2;
};

class IAudioSource {
 public:
  virtual ~IAudioSource() = default;
  virtual bool start(std::string& err) = 0;
  virtual void stop() = 0;
  virtual std::string name() const = 0;
  AudioRing& ring() { return ring_; }

 protected:
  AudioRing ring_;
};

std::unique_ptr<IVideoSource> makeVideoSource(const RecordConfig& c);
std::unique_ptr<IAudioSource> makeSysSource(const RecordConfig& c);  // null when sys == none
std::unique_ptr<IAudioSource> makeMicSource(const RecordConfig& c);  // null when mic == none

// Shared phase of the synthetic sources: a flash and a beep during the first 100 ms of each second.
void setSynthEpoch(int64_t us);
bool synthFlashAt(int64_t us);

}  // namespace vv
