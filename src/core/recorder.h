#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "config.h"
#include "encoder.h"
#include "sources.h"

namespace vv {

enum class RecState { Idle, Starting, Recording, Paused, Stopping };
const char* stateName(RecState s);

struct RecResult {
  std::string path, encoder, error, telemetryDir;
  double durationSec = 0;
  uint64_t videoFrames = 0, repeatedFrames = 0, droppedTicks = 0, bytes = 0;
  int width = 0, height = 0;
  json toJson() const;
};

// Owns the whole pipeline: sources -> timeline -> muxer. One recording at a time.
//
// Timeline: a single QPC clock; pauses are subtracted so the file has no hole. Video is paced
// at a constant frame rate (the last frame repeats while the source is static). Audio tracks
// are kept sample-exact against the same clock: silence fills the gaps that loopback capture
// leaves when nothing plays, and slow drift is trimmed.
class Recorder {
 public:
  Recorder();
  ~Recorder();

  bool start(const RecordConfig& cfg, std::string& err);
  bool pause();
  bool resume();
  RecResult stop();

  RecState state() const { return state_; }
  int64_t elapsedUs() const;  // recording time, pauses excluded
  json status() const;
  RecResult lastResult() const;
  std::string lastError() const;

  std::function<void(RecState)> onState;  // may be called from any thread

 private:
  struct Track {
    const char* label = "";
    std::unique_ptr<IAudioSource> src;
    int index = -1;
    int64_t written = 0;     // frames handed to the muxer
    int64_t padded = 0;      // frames of inserted silence
    int64_t trimmed = 0;     // frames dropped to cancel drift
    double peak = 0, sumSq = 0, maxPeak = 0;
    int64_t meterN = 0;
  };

  void setState(RecState s);
  void videoLoop();
  void audioLoop();
  void feedTrack(Track& t, bool paused);
  void publishMeter(Track& t);
  void handleSourceClosed(const std::string& why);
  RecResult finish(const std::string& error);

  mutable std::mutex mu_;       // start/stop/pause serialisation
  std::mutex closerMu_;
  mutable std::mutex timeMu_;   // guards the timeline fields
  std::atomic<RecState> state_{RecState::Idle};
  RecordConfig cfg_;
  std::string path_, telemetryDir_;
  std::unique_ptr<Muxer> mux_;
  std::unique_ptr<IVideoSource> video_;
  Track sys_, mic_;
  std::thread vth_, ath_, closer_;
  std::atomic<bool> run_{false};

  int64_t t0_ = 0, pausedTotal_ = 0, pauseStart_ = 0;
  bool paused_ = false;

  std::mutex frameMu_;
  FramePtr latest_;
  std::atomic<uint64_t> latestSeq_{0}, sourceFrames_{0};

  std::atomic<uint64_t> repeated_{0}, droppedTicks_{0};
  std::string error_;
  RecResult last_;
  mutable std::mutex errMu_;
  bool timerRaised_ = false;
};

}  // namespace vv
