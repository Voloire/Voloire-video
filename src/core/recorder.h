#pragma once
#include <atomic>
#include <condition_variable>
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

  // One-shot key for this process (CLI --stream-key); when empty, the stored DPAPI secret is
  // used. Never persisted, never logged, never part of RecordConfig.
  void setStreamKey(const std::string& k) { streamKeyOverride_ = k; }

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
    std::vector<float> outq;  // what was written, waiting to be mixed into the MIX track
  };

  void setState(RecState s);
  void videoLoop();
  void audioLoop();
  void feedTrack(Track& t, bool paused);
  void publishMeter(Track& t);
  void mixTick();
  void handleSourceClosed(const std::string& why);
  RecResult finish(const std::string& error);

  std::shared_ptr<Muxer> muxSnapshot() const;
  void streamTick(const std::shared_ptr<Muxer>& m);
  void setStreamState(const std::string& s);
  void ensureReconnect(const std::string& why);
  void reconnectLoop();
  json streamStatus() const;

  mutable std::mutex mu_;       // start/stop/pause serialisation
  std::mutex closerMu_;
  mutable std::mutex timeMu_;   // guards the timeline fields
  std::atomic<RecState> state_{RecState::Idle};
  RecordConfig cfg_;
  std::string path_, telemetryDir_, streamTarget_;
  std::shared_ptr<Muxer> mux_;  // swapped by the reconnect thread under muxMu_
  mutable std::mutex muxMu_;
  EncoderParams streamParams_;  // the same settings for every reconnect attempt
  std::unique_ptr<IVideoSource> video_;
  Track sys_, mic_;
  std::thread vth_, ath_, closer_, rth_;
  std::atomic<bool> run_{false};
  std::mutex runMu_;
  std::condition_variable runCv_;  // wakes the reconnect thread at stop

  int64_t t0_ = 0, pausedTotal_ = 0, pauseStart_ = 0;
  bool paused_ = false;

  int mixIndex_ = -1;  // muxer track of the SYS+MIC mix, -1 when only one channel is recorded
  int64_t mixWritten_ = 0;  // stream mix frames written, for the silence-on-the-clock path

  std::mutex frameMu_;
  FramePtr latest_;
  std::atomic<uint64_t> latestSeq_{0}, sourceFrames_{0};

  std::atomic<uint64_t> repeated_{0}, droppedTicks_{0};
  std::string error_;
  RecResult last_;
  mutable std::mutex errMu_;
  bool timerRaised_ = false;

  // Stream bookkeeping, guarded by streamMu_ (atomics excepted).
  mutable std::mutex streamMu_;
  std::string streamState_ = "ended";  // connecting | live | reconnecting | ended
  std::string streamKeyOverride_;
  std::atomic<bool> reconnectActive_{false};
  std::atomic<uint64_t> reconnects_{0};
  std::atomic<uint64_t> retiredBytes_{0}, retiredDropped_{0};  // left behind by a reconnect
  int64_t kbpsT_ = 0;
  uint64_t kbpsB_ = 0;
  double streamKbps_ = 0;
};

}  // namespace vv
