#include "recorder.h"
#include <windows.h>
#include <timeapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "quality.h"
#include "redact.h"
#include "secrets.h"
#include "telemetry.h"

namespace vv {

namespace fs = std::filesystem;

const char* stateName(RecState s) {
  switch (s) {
    case RecState::Idle: return "idle";
    case RecState::Starting: return "starting";
    case RecState::Recording: return "recording";
    case RecState::Paused: return "paused";
    default: return "stopping";
  }
}

json RecResult::toJson() const {
  return {{"path", path},           {"encoder", encoder},         {"error", error},
          {"telemetryDir", telemetryDir}, {"durationSec", durationSec}, {"videoFrames", videoFrames},
          {"repeatedFrames", repeatedFrames}, {"droppedTicks", droppedTicks}, {"bytes", bytes},
          {"width", width},         {"height", height}};
}

namespace {
double toDb(double lin) { return lin <= 1e-6 ? -120.0 : 20.0 * std::log10(lin); }

std::string timestamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
  return b;
}
}  // namespace

Recorder::Recorder() = default;

Recorder::~Recorder() {
  if (state_ != RecState::Idle) stop();
  if (rth_.joinable()) rth_.join();  // never under closerMu_: the loop takes it at its end
  std::lock_guard cl(closerMu_);
  if (closer_.joinable()) closer_.join();
}

void Recorder::setState(RecState s) {
  state_ = s;
  VV_LOG(Info, "recorder", std::string("state ") + stateName(s));
  if (onState) onState(s);
}

int64_t Recorder::elapsedUs() const {
  std::lock_guard lk(timeMu_);
  if (state_ == RecState::Idle || state_ == RecState::Starting) return 0;
  int64_t ref = paused_ ? pauseStart_ : nowUs();
  return std::max<int64_t>(0, ref - t0_ - pausedTotal_);
}

std::string Recorder::lastError() const {
  std::lock_guard lk(errMu_);
  return error_;
}

bool Recorder::start(const RecordConfig& cfg, std::string& err) {
  std::lock_guard lk(mu_);
  if (state_ != RecState::Idle) { err = "already recording"; return false; }
  {
    std::lock_guard cl(closerMu_);
    if (closer_.joinable()) closer_.join();
  }
  err = cfg.validate();
  if (!err.empty()) return false;
  if (cfg.videoSource == "window" && !IsWindow((HWND)(intptr_t)cfg.hwnd)) { err = "selected window no longer exists"; return false; }
  const bool streaming = cfg.output == "stream";
  if (streaming) {
    std::string key = streamKeyOverride_;
    if (key.empty()) key = loadSecret("stream_key");
    if (key.empty()) {
      err = "no stream key: set it in YouTube > Set stream key, or pass --stream-key";
      return false;  // still before any source starts and before anything touches the network
    }
    streamTarget_ = cfg.streamUrl + "/" + key;
  }

  state_ = RecState::Starting;
  cfg_ = cfg;
  { std::lock_guard e(errMu_); error_.clear(); }
  repeated_ = 0; droppedTicks_ = 0; sourceFrames_ = 0; latestSeq_ = 0; latest_.reset();
  sys_ = Track{}; mic_ = Track{};
  sys_.label = "sys"; mic_.label = "mic";
  reconnects_ = 0;
  retiredBytes_ = 0; retiredDropped_ = 0;
  mixWritten_ = 0;
  reconnectActive_ = false;
  {
    std::lock_guard lk(streamMu_);
    kbpsT_ = 0; kbpsB_ = 0; streamKbps_ = 0;
    streamState_ = streaming ? "connecting" : "ended";
  }
  if (streaming) VV_LOG(Info, "stream", "stream: connecting");

  std::string base = cfg.name + "_" + timestamp();
  std::string outDir = cfg.outDir.empty() ? defaultOutDir() : cfg.outDir;
  try { fs::create_directories(pathOf(outDir)); } catch (...) {}
  if (streaming) path_.clear();  // a stream writes no file
  else path_ = strOf(pathOf(outDir) / pathOf(base + "." + cfg.container));
  telemetryDir_ = cfg.telemetryDir.empty() ? strOf(pathOf(outDir) / "telemetry" / pathOf(base))
                                           : cfg.telemetryDir;
  auto& tel = Telemetry::get();
  tel.resetMetrics();
  tel.startSession(telemetryDir_);
  VV_LOG(Info, "recorder", "state starting");  // first state of the session log
  if (onState) onState(RecState::Starting);
  VV_LOG(Info, "recorder", "start requested", cfg.toJson());

  auto fail = [&](const std::string& why) {
    err = why;
    VV_LOG(Error, "recorder", "start failed", {{"why", why}});
    if (video_) { video_->stop(); video_.reset(); }
    for (Track* t : {&sys_, &mic_}) if (t->src) { t->src->stop(); t->src.reset(); }
    tel.endSession();
    setState(RecState::Idle);
    return false;
  };

  setSynthEpoch(nowUs());
  video_ = makeVideoSource(cfg);
  if (!video_) return fail("no video source");
  std::string e;
  auto sink = [this](FramePtr f) {
    {
      std::lock_guard lk2(frameMu_);
      latest_ = std::move(f);
    }
    ++latestSeq_;
    ++sourceFrames_;
    Telemetry::get().inc("video.source_frames");
  };
  if (!video_->start(sink, [this](const std::string& why) { handleSourceClosed(why); }, e)) return fail("video: " + e);
  VV_LOG(Info, "recorder", "video source started", {{"name", video_->name()}});

  sys_.src = makeSysSource(cfg);
  mic_.src = makeMicSource(cfg);
  for (Track* t : {&sys_, &mic_}) {
    if (!t->src) continue;
    if (!t->src->start(e)) return fail(std::string(t->label) + ": " + e);
    VV_LOG(Info, "recorder", "audio source started", {{"track", t->label}, {"name", t->src->name()}});
  }

  // Wait for the first frame: it fixes the output size.
  FramePtr first;
  for (int i = 0; i < 100 && !first; ++i) {
    {
      std::lock_guard lk2(frameMu_);
      first = latest_;
    }
    if (!first) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!first) return fail("no frame received from the video source within 5 s");

  EncoderParams ep;
  ep.codec = cfg.codec;
  ep.encoder = cfg.encoder;
  ep.fps = cfg.fps;
  int sw = first->w & ~1, sh = first->h & ~1;
  if (streaming) {
    // Live stream: CBR H.264 in FLV over RTMP, one MIX audio track carrying the sum of the
    // enabled channels (silence when none: YouTube needs an audio track).
    ep.path = streamTarget_;
    ep.format = "flv";
    ep.container = "flv";
    ep.codec = "h264";
    ep.rateControl = "cbr";
    ep.gopSeconds = 2;
    ep.audioKbps = 128;
    ep.asyncWrite = true;
    ep.maxQueueMs = 3000;
    ep.startPtsUs = 0;
    ep.bitrateKbps = cfg.streamBitrateKbps;
    ep.width = sw;
    ep.height = sh;
    if (cfg.streamHeight > 0 && ep.height > cfg.streamHeight) {
      ep.height = cfg.streamHeight & ~1;
      ep.width = (int)std::lround((double)sw * ep.height / sh) & ~1;
    }
    ep.audioTracks.push_back("MIX");
    mixIndex_ = 0;
  } else {
    ep.path = path_;
    ep.container = cfg.container;
    if (cfg.outHeight > 0 && cfg.outHeight != sh) {
      ep.height = cfg.outHeight & ~1;
      ep.width = (int)std::lround((double)sw * ep.height / sh) & ~1;
    } else {
      ep.width = sw;
      ep.height = sh;
    }
    // Quality presets cap the height and resolve the bitrate once the output size is known.
    int heightCap = presetHeightCap(cfg.quality);
    if (heightCap > 0 && ep.height > heightCap) {
      ep.height = heightCap;
      ep.width = (int)std::lround((double)sw * ep.height / sh) & ~1;
    }
    ep.bitrateKbps = cfg.quality == "custom" ? cfg.bitrateKbps
                                             : presetBitrateKbps(cfg.quality, ep.height, cfg.fps, cfg.codec);
    // With both channels, the first (default) track is their mix: players play only one track,
    // and the separate SYS and MIC tracks stay for editing.
    mixIndex_ = -1;
    if (sys_.src && mic_.src) { mixIndex_ = (int)ep.audioTracks.size(); ep.audioTracks.push_back("MIX"); }
    if (sys_.src) { sys_.index = (int)ep.audioTracks.size(); ep.audioTracks.push_back("SYS"); }
    if (mic_.src) { mic_.index = (int)ep.audioTracks.size(); ep.audioTracks.push_back("MIC"); }
  }
  tel.set("video.bitrate_kbps", (double)ep.bitrateKbps);

  streamParams_ = ep;  // every reconnect attempt opens a fresh muxer with these settings
  mux_ = std::make_shared<Muxer>();
  if (!mux_->open(ep, e)) { mux_.reset(); return fail(streaming ? "stream: " + e : "encoder: " + e); }
  VV_LOG(Info, "recorder", "muxer open",
         {{"path", streaming ? redactUrl(streamTarget_) : path_}, {"encoder", mux_->videoEncoderName()},
          {"w", ep.width}, {"h", ep.height}, {"fps", ep.fps}, {"bitrateKbps", ep.bitrateKbps}});
  if (streaming) setStreamState("live");

  if (!timerRaised_) { timeBeginPeriod(1); timerRaised_ = true; }
  {
    std::lock_guard tl(timeMu_);
    t0_ = nowUs();
    pausedTotal_ = 0;
    paused_ = false;
  }
  for (Track* t : {&sys_, &mic_}) if (t->src) t->src->ring().clear();
  run_ = true;
  vth_ = std::thread([this] { videoLoop(); });
  ath_ = std::thread([this] { audioLoop(); });
  setState(RecState::Recording);
  return true;
}

bool Recorder::pause() {
  std::lock_guard lk(mu_);
  if (cfg_.output == "stream") return false;  // a live stream has no pause
  if (state_ != RecState::Recording) return false;
  {
    std::lock_guard tl(timeMu_);
    pauseStart_ = nowUs();
    paused_ = true;
  }
  setState(RecState::Paused);
  return true;
}

bool Recorder::resume() {
  std::lock_guard lk(mu_);
  if (state_ != RecState::Paused) return false;
  {
    std::lock_guard tl(timeMu_);
    pausedTotal_ += nowUs() - pauseStart_;
    paused_ = false;
  }
  for (Track* t : {&sys_, &mic_}) if (t->src) t->src->ring().clear();
  setState(RecState::Recording);
  return true;
}

void Recorder::handleSourceClosed(const std::string& why) {
  {
    std::lock_guard e(errMu_);
    if (error_.empty()) error_ = why;
  }
  VV_LOG(Warn, "recorder", "video source closed, finalising", {{"why", why}});
  // The notification comes from the source's own thread, which stop() would join: hop off it.
  std::lock_guard lk(closerMu_);
  RecState st = state_;
  if ((st != RecState::Recording && st != RecState::Paused) || closer_.joinable()) return;
  closer_ = std::thread([this] { stop(); });
}

void Recorder::videoLoop() {
  const int64_t period = 1000000 / cfg_.fps;
  const bool streaming = cfg_.output == "stream";
  int64_t next = nowUs();
  uint64_t lastSeq = ~0ull;
  uint64_t frames = 0, winFrames = 0;
  int64_t winStart = nowUs();
  double encEma = 0;
  auto& tel = Telemetry::get();
  while (run_) {
    int64_t now = nowUs();
    bool paused;
    { std::lock_guard tl(timeMu_); paused = paused_; }
    if (paused) {
      next = now + period;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    if (now < next) {
      std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(next - now, 4000)));
      continue;
    }
    if (now > next + 3 * period) {  // fell behind: skip ticks, never burst
      uint64_t skipped = (uint64_t)((now - next) / period);
      droppedTicks_ += skipped;
      tel.inc("video.dropped_ticks", (double)skipped);
      next = now;
    }
    next += period;

    FramePtr f;
    { std::lock_guard lk2(frameMu_); f = latest_; }
    if (!f) continue;
    uint64_t seq = latestSeq_;
    if (seq == lastSeq) { ++repeated_; tel.inc("video.repeated_frames"); }
    lastSeq = seq;

    int64_t pts = elapsedUs();
    int64_t a = nowUs();
    auto m = muxSnapshot();
    if (!m) {
      // Reconnecting: the timeline keeps running, the frame is dropped.
    } else if (streaming && m->failed()) {
      ensureReconnect("the stream write failed");
    } else if (!m->writeVideo(*f, pts)) {
      tel.inc("video.encode_errors");
    }
    double ms = (nowUs() - a) / 1000.0;
    encEma = encEma == 0 ? ms : encEma * 0.9 + ms * 0.1;
    ++frames; ++winFrames;
    tel.inc("video.frames");
    tel.set("video.encode_ms", encEma);
    tel.set("video.queue_lag_ms", std::max<int64_t>(0, nowUs() - next) / 1000.0);
    if (nowUs() - winStart >= 1000000) {
      tel.set("video.fps_actual", winFrames * 1e6 / (double)(nowUs() - winStart));
      tel.set("video.source_fps_total", (double)sourceFrames_);
      tel.set("rec.size_bytes", (double)mux_->bytesWritten());
      if (streaming) streamTick(m);
      winFrames = 0;
      winStart = nowUs();
    }
  }
}

void Recorder::publishMeter(Track& t) {
  if (!t.meterN) return;
  auto& tel = Telemetry::get();
  std::string k = std::string("audio.") + t.label;
  tel.set(k + ".rms_db", toDb(std::sqrt(t.sumSq / (double)t.meterN)));
  t.maxPeak = std::max(t.maxPeak, t.peak);
  tel.set(k + ".peak_db", toDb(t.peak));
  tel.set(k + ".max_peak_db", toDb(t.maxPeak));  // since the start: a beep every second would otherwise hide
  tel.set(k + ".padded_ms", t.padded * 1000.0 / kSampleRate);
  tel.set(k + ".trimmed_ms", t.trimmed * 1000.0 / kSampleRate);
  tel.set(k + ".written_s", (double)t.written / kSampleRate);
  tel.set(k + ".ring_ms", t.src ? t.src->ring().available() * 1000.0 / kSampleRate : 0.0);
  t.peak = 0; t.sumSq = 0; t.meterN = 0;
}

void Recorder::feedTrack(Track& t, bool paused) {
  if (!t.src) return;
  if (paused) { t.src->ring().clear(); t.outq.clear(); return; }
  auto m = muxSnapshot();
  const bool streaming = cfg_.output == "stream";
  const bool live = m && !(streaming && m->failed());  // a dead stream stops being fed, keeps the clock
  const int64_t expected = elapsedUs() * kSampleRate / 1000000;
  const int64_t slack = kSampleRate * 60 / 1000;   // 60 ms
  std::vector<float> buf;
  size_t avail = t.src->ring().available();
  if (avail) {
    buf.resize(avail * kChannels);
    size_t n = t.src->ring().pop(buf.data(), avail);
    // Source running ahead of the clock (its crystal is faster): trim the excess.
    int64_t ahead = t.written + (int64_t)n - expected;
    size_t skip = 0;
    if (ahead > kSampleRate * 120 / 1000) {
      skip = (size_t)std::min<int64_t>((int64_t)n, ahead - kSampleRate * 40 / 1000);
      t.trimmed += (int64_t)skip;
    }
    if (n > skip) {
      const float* p = buf.data() + skip * kChannels;
      size_t m2 = n - skip;
      for (size_t i = 0; i < m2 * kChannels; ++i) {
        double v = std::fabs(p[i]);
        t.peak = std::max(t.peak, v);
        t.sumSq += v * v;
      }
      t.meterN += (int64_t)(m2 * kChannels);
      if (live) {
        if (t.index >= 0) m->writeAudio(t.index, p, m2);
        if (mixIndex_ >= 0) t.outq.insert(t.outq.end(), p, p + m2 * kChannels);
      } else if (mixIndex_ >= 0) {
        t.outq.clear();  // disconnected: what cannot be sent must not pile up for later
      }
      t.written += (int64_t)m2;
    }
  } else if (expected - t.written > slack) {
    // Nothing captured (loopback is silent when nothing plays): keep the track on the clock.
    size_t pad = (size_t)(expected - t.written - kSampleRate * 10 / 1000);
    if (live) {
      if (t.index >= 0) {
        std::vector<float> zeros(pad * kChannels, 0.f);
        m->writeAudio(t.index, zeros.data(), pad);
      }
      if (mixIndex_ >= 0) t.outq.insert(t.outq.end(), pad * kChannels, 0.f);
    } else if (mixIndex_ >= 0) {
      t.outq.clear();
    }
    t.written += (int64_t)pad;
    t.padded += (int64_t)pad;
    t.meterN += (int64_t)(pad * kChannels);
  }
}

void Recorder::mixTick() {
  if (mixIndex_ < 0) return;
  auto m = muxSnapshot();
  if (cfg_.output != "stream") {
    // File mode: both queues are padded by feedTrack and stay in step, mix what they both have.
    if (!m) return;
    size_t n = std::min(sys_.outq.size(), mic_.outq.size()) / kChannels;
    if (!n) return;
    std::vector<float> mix(n * kChannels);
    for (size_t i = 0; i < mix.size(); ++i) mix[i] = std::clamp(sys_.outq[i] + mic_.outq[i], -1.0f, 1.0f);
    m->writeAudio(mixIndex_, mix.data(), n);
    sys_.outq.erase(sys_.outq.begin(), sys_.outq.begin() + (ptrdiff_t)(n * kChannels));
    mic_.outq.erase(mic_.outq.begin(), mic_.outq.begin() + (ptrdiff_t)(n * kChannels));
    return;
  }
  const int64_t expected = elapsedUs() * kSampleRate / 1000000;
  if (!m || m->failed()) {
    mixWritten_ = expected;  // the clock advances, so nothing bursts in after a reconnect
    sys_.outq.clear();
    mic_.outq.clear();
    return;
  }
  size_t nsys = sys_.outq.size(), nmic = mic_.outq.size();
  size_t n = std::max(nsys, nmic) / kChannels;
  if (!n) {
    // No channel at all: a stream still needs audio on the clock (YouTube stops on silence-less).
    if (expected - mixWritten_ > kSampleRate * 40 / 1000) {
      size_t pad = (size_t)(expected - mixWritten_ - kSampleRate * 10 / 1000);
      std::vector<float> zeros(pad * kChannels, 0.f);
      m->writeAudio(mixIndex_, zeros.data(), pad);
      mixWritten_ += (int64_t)pad;
    }
    return;
  }
  std::vector<float> mix(n * kChannels);
  for (size_t i = 0; i < mix.size(); ++i) {
    float a = i < nsys ? sys_.outq[i] : 0.f;
    float b = i < nmic ? mic_.outq[i] : 0.f;
    mix[i] = std::clamp(a + b, -1.0f, 1.0f);
  }
  m->writeAudio(mixIndex_, mix.data(), n);
  mixWritten_ += (int64_t)n;
  sys_.outq.erase(sys_.outq.begin(), sys_.outq.begin() + (ptrdiff_t)std::min(nsys, n * kChannels));
  mic_.outq.erase(mic_.outq.begin(), mic_.outq.begin() + (ptrdiff_t)std::min(nmic, n * kChannels));
}

void Recorder::audioLoop() {
  int64_t lastMeter = nowUs();
  while (run_) {
    bool paused;
    { std::lock_guard tl(timeMu_); paused = paused_; }
    feedTrack(sys_, paused);
    feedTrack(mic_, paused);
    mixTick();
    if (nowUs() - lastMeter >= 250000) {
      publishMeter(sys_);
      publishMeter(mic_);
      lastMeter = nowUs();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

RecResult Recorder::stop() {
  std::lock_guard lk(mu_);
  if (state_ == RecState::Idle) return {};
  setState(RecState::Stopping);
  RecResult r;
  r.durationSec = (double)elapsedUs() / 1e6;
  run_ = false;
  runCv_.notify_all();
  if (vth_.joinable()) vth_.join();
  if (ath_.joinable()) ath_.join();
  if (rth_.joinable()) rth_.join();
  VV_LOG(Debug, "recorder", "worker threads joined");
  if (video_) { video_->stop(); video_.reset(); }
  VV_LOG(Debug, "recorder", "video source stopped");
  for (Track* t : {&sys_, &mic_}) if (t->src) { t->src->stop(); }
  const bool streaming = cfg_.output == "stream";
  std::string err = lastError();
  uint64_t sent = 0;
  if (mux_) {
    r.videoFrames = mux_->videoFramesWritten();
    r.encoder = mux_->videoEncoderName();
    r.width = mux_->width();
    r.height = mux_->height();
    sent = retiredBytes_ + mux_->bytesWritten();
    std::string e;
    if (!mux_->close(e)) { if (err.empty()) err = e; }
    VV_LOG(Debug, "recorder", "muxer closed");
    mux_.reset();
  }
  for (Track* t : {&sys_, &mic_}) t->src.reset();
  r.path = path_;
  r.telemetryDir = telemetryDir_;
  r.repeatedFrames = repeated_;
  r.droppedTicks = droppedTicks_;
  r.error = err;
  if (streaming) {
    r.bytes = sent;  // a stream counts what it sent, there is no file to size
    Telemetry::get().set("stream.bytes", (double)sent);
  } else {
    try { r.bytes = fs::exists(pathOf(path_)) ? (uint64_t)fs::file_size(pathOf(path_)) : 0; } catch (...) {}
  }
  json res = r.toJson();
  res["metrics"] = Telemetry::get().snapshot();
  VV_LOG(Info, "recorder", "recording finished", res);
  try { std::ofstream(pathOf(telemetryDir_) / "result.json") << res.dump(2); } catch (...) {}
  { std::lock_guard tl(timeMu_); paused_ = false; }
  { std::lock_guard el(errMu_); last_ = r; }
  if (streaming) setStreamState("ended");
  setState(RecState::Idle);  // logged before the session files close
  Telemetry::get().endSession();
  if (timerRaised_) { timeEndPeriod(1); timerRaised_ = false; }
  return r;
}

RecResult Recorder::lastResult() const {
  std::lock_guard el(errMu_);
  return last_;
}

json Recorder::status() const {
  json j = {{"state", stateName(state_)}, {"elapsedUs", elapsedUs()}, {"path", path_},
            {"error", lastError()}, {"metrics", Telemetry::get().snapshot()}};
  j["output"] = cfg_.output;
  j["stream"] = streamStatus();
  return j;
}

std::shared_ptr<Muxer> Recorder::muxSnapshot() const {
  std::lock_guard lk(muxMu_);
  return mux_;
}

void Recorder::setStreamState(const std::string& s) {
  {
    std::lock_guard lk(streamMu_);
    streamState_ = s;
  }
  VV_LOG(Info, "stream", "stream: " + s);
}

json Recorder::streamStatus() const {
  std::shared_ptr<Muxer> m = muxSnapshot();
  std::lock_guard lk(streamMu_);
  return {{"state", streamState_},
          {"kbps", streamKbps_},
          {"queueMs", m ? m->queuedMs() : 0.0},
          {"droppedPackets", retiredDropped_.load() + (m ? m->droppedPackets() : 0)},
          {"reconnects", reconnects_.load()}};
}

void Recorder::streamTick(const std::shared_ptr<Muxer>& m) {
  const int64_t now = nowUs();
  const uint64_t b = retiredBytes_.load() + (m ? m->bytesWritten() : 0);
  double kbps;
  {
    std::lock_guard lk(streamMu_);
    if (kbpsT_) {
      double dt = (now - kbpsT_) / 1e6;
      if (dt > 0) streamKbps_ = (double)(b - kbpsB_) * 8.0 / 1000.0 / dt;
    }
    kbpsT_ = now;
    kbpsB_ = b;
    kbps = streamKbps_;
  }
  auto& tel = Telemetry::get();
  tel.set("stream.kbps", kbps);
  tel.set("stream.queue_ms", m ? m->queuedMs() : 0.0);
  tel.set("stream.dropped_packets", (double)(retiredDropped_.load() + (m ? m->droppedPackets() : 0)));
  tel.set("stream.reconnects", (double)reconnects_.load());
  tel.set("stream.bytes", (double)b);
}

void Recorder::ensureReconnect(const std::string& why) {
  if (reconnectActive_.exchange(true)) return;
  VV_LOG(Warn, "stream", "stream write failed", {{"why", why}});
  setStreamState("reconnecting");
  if (rth_.joinable()) rth_.join();  // the previous loop has already returned
  rth_ = std::thread([this] { reconnectLoop(); });
}

void Recorder::reconnectLoop() {
  static const int kDelays[5] = {1, 2, 4, 8, 16};
  std::string why = "connection lost";
  for (int i = 0; i < 5; ++i) {
    {
      std::unique_lock lk(runMu_);
      if (runCv_.wait_for(lk, std::chrono::seconds(kDelays[i]), [this] { return !run_.load(); })) return;
    }
    std::string e;
    auto nm = std::make_shared<Muxer>();
    EncoderParams ep = streamParams_;
    ep.startPtsUs = elapsedUs();  // the fresh stream picks the timeline up where it stands
    VV_LOG(Info, "stream", "reconnect attempt", {{"attempt", i + 1}});
    if (nm->open(ep, e)) {
      std::shared_ptr<Muxer> old;
      {
        std::lock_guard lk(muxMu_);
        old = mux_;
        mux_ = nm;
      }
      if (old) {  // retire outside the lock: the destructor may join the old writer
        retiredBytes_ += old->bytesWritten();
        retiredDropped_ += old->droppedPackets();
        old.reset();
      }
      ++reconnects_;
      Telemetry::get().set("stream.reconnects", (double)reconnects_.load());
      setStreamState("live");
      reconnectActive_ = false;
      return;
    }
    why = e;
    VV_LOG(Warn, "stream", "reconnect attempt failed", {{"attempt", i + 1}, {"why", e}});
  }
  if (!run_.load()) return;
  {
    std::lock_guard e(errMu_);
    if (error_.empty()) error_ = "stream lost: " + why;
  }
  VV_LOG(Error, "stream", "stream lost", {{"why", why}});
  setStreamState("ended");
  reconnectActive_ = false;
  std::lock_guard lk(closerMu_);
  if (state_ == RecState::Recording && !closer_.joinable()) closer_ = std::thread([this] { stop(); });
}

}  // namespace vv
