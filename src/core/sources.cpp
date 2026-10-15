#include "sources.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "devices.h"
#include "telemetry.h"
#include "wasapi_source.h"
#include "video_sources.h"

namespace vv {

// ---------------------------------------------------------------- AudioRing

void AudioRing::push(const float* in, size_t frames) {
  std::lock_guard lk(mu_);
  buf_.insert(buf_.end(), in, in + frames * kChannels);
  size_t have = (buf_.size() - head_) / kChannels;
  if (have > kMaxFrames) {
    size_t drop = have - kMaxFrames;
    head_ += drop * kChannels;
    dropped_ += drop;
  }
  if (head_ > buf_.size() / 2 && head_ > 65536) {
    buf_.erase(buf_.begin(), buf_.begin() + head_);
    head_ = 0;
  }
}

size_t AudioRing::pop(float* dst, size_t maxFrames) {
  std::lock_guard lk(mu_);
  size_t have = (buf_.size() - head_) / kChannels;
  size_t n = std::min(have, maxFrames);
  std::memcpy(dst, buf_.data() + head_, n * kChannels * sizeof(float));
  head_ += n * kChannels;
  if (head_ == buf_.size()) { buf_.clear(); head_ = 0; }
  return n;
}

size_t AudioRing::available() const {
  std::lock_guard lk(mu_);
  return (buf_.size() - head_) / kChannels;
}

void AudioRing::clear() {
  std::lock_guard lk(mu_);
  buf_.clear();
  head_ = 0;
}

// ------------------------------------------------------------ synthetic

namespace {
std::atomic<int64_t> g_epoch{0};
}

void setSynthEpoch(int64_t us) { g_epoch = us; }

bool synthFlashAt(int64_t us) {
  int64_t t = us - g_epoch.load();
  if (t < 0) return false;
  return (t % 1000000) < 100000;
}

namespace {

// Generates BGRA test frames: a 24-bit frame counter in the top strip, a moving bar,
// and a white flash on the beat shared with the synthetic beep.
class SynthVideoSource : public IVideoSource {
 public:
  explicit SynthVideoSource(int fps) : fps_(std::max(1, fps)) {}
  ~SynthVideoSource() override { stop(); }

  bool start(std::function<void(FramePtr)> sink, std::function<void(const std::string&)>, std::string&) override {
    sink_ = std::move(sink);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() override {
    run_ = false;
    if (th_.joinable()) th_.join();
  }
  std::string name() const override { return "synthetic video 1280x720"; }

 private:
  static constexpr int W = 1280, H = 720, kBits = 24, kCell = 20, kStrip = 24;

  void loop() {
    const int64_t period = 1000000 / fps_;
    int64_t next = nowUs();
    uint64_t seq = 0;
    while (run_) {
      int64_t now = nowUs();
      if (now < next) {
        std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(next - now, 2000)));
        continue;
      }
      next += period;
      auto f = std::make_shared<VideoFrame>();
      f->w = W; f->h = H; f->seq = seq; f->capUs = now;
      f->bgra.resize((size_t)W * H * 4);
      render(*f, seq, synthFlashAt(now));
      sink_(std::move(f));
      ++seq;
    }
  }

  static void render(VideoFrame& f, uint64_t seq, bool flash) {
    uint8_t* p = f.bgra.data();
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x, p += 4) {
        if (y < kStrip) {
          int bit = x / kCell;
          bool on = bit < kBits && ((seq >> (kBits - 1 - bit)) & 1);
          uint8_t v = on ? 255 : 0;
          p[0] = p[1] = p[2] = v;
        } else if (flash) {
          p[0] = p[1] = p[2] = 255;
        } else {
          p[0] = (uint8_t)(60 + 120 * y / H);  // blue gradient
          p[1] = (uint8_t)(10 + 30 * x / W);
          p[2] = 8;
        }
        p[3] = 255;
      }
    }
    if (!flash) {  // moving neon bar
      int bx = (int)((seq * 8) % (W - 40));
      for (int y = kStrip + 100; y < kStrip + 160; ++y) {
        uint8_t* q = f.bgra.data() + ((size_t)y * W + bx) * 4;
        for (int x = 0; x < 40; ++x, q += 4) { q[0] = 255; q[1] = 240; q[2] = 0; }
      }
    }
  }

  int fps_;
  std::atomic<bool> run_{false};
  std::thread th_;
  std::function<void(FramePtr)> sink_;
};

// Realtime paced tone generator. beep=true gates the tone to the flash window.
class SynthAudioSource : public IAudioSource {
 public:
  SynthAudioSource(std::string label, double hz, bool beep, float amp)
      : label_(std::move(label)), hz_(hz), beep_(beep), amp_(amp) {}
  ~SynthAudioSource() override { stop(); }

  bool start(std::string&) override {
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() override {
    run_ = false;
    if (th_.joinable()) th_.join();
  }
  std::string name() const override { return label_; }

 private:
  void loop() {
    const int64_t t0 = nowUs();
    uint64_t idx = 0;
    std::vector<float> buf;
    while (run_) {
      int64_t target = (nowUs() - t0) * kSampleRate / 1000000;
      if ((int64_t)idx >= target) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      size_t n = (size_t)std::min<int64_t>(target - (int64_t)idx, 2400);
      buf.resize(n * kChannels);
      for (size_t i = 0; i < n; ++i) {
        int64_t tUs = t0 + (int64_t)(idx + i) * 1000000 / kSampleRate;
        float s = 0.f;
        if (!beep_ || synthFlashAt(tUs))
          s = amp_ * (float)std::sin(2.0 * 3.14159265358979 * hz_ * (double)(idx + i) / kSampleRate);
        buf[i * 2] = buf[i * 2 + 1] = s;
      }
      idx += n;
      ring_.push(buf.data(), n);
    }
  }

  std::string label_;
  double hz_;
  bool beep_;
  float amp_;
  std::atomic<bool> run_{false};
  std::thread th_;
};

}  // namespace

// ---------------------------------------------------------------- factories

std::unique_ptr<IVideoSource> makeVideoSource(const RecordConfig& c) {
  if (c.videoSource == "synthetic") return std::make_unique<SynthVideoSource>(c.synthFps);
  if (c.videoSource == "window") return makeWgcWindowSource((void*)(intptr_t)c.hwnd);
  if (c.videoSource == "monitor") return makeWgcMonitorSource(c.monitor);
  if (c.videoSource == "dxgi") return makeDxgiSource(c.monitor);
  return nullptr;
}

std::unique_ptr<IAudioSource> makeSysSource(const RecordConfig& c) {
  if (c.sys == "none") return nullptr;
  if (c.sys == "synthetic") return std::make_unique<SynthAudioSource>("synthetic SYS tone", c.synthSysHz, true, 0.5f);
  if (c.sys == "app") return makeProcessLoopbackSource(c.pid);
  return makeEndpointSource(c.sysDevice, /*loopback=*/true);
}

std::unique_ptr<IAudioSource> makeMicSource(const RecordConfig& c) {
  if (c.mic == "none") return nullptr;
  if (c.mic == "synthetic") return std::make_unique<SynthAudioSource>("synthetic MIC tone", c.synthMicHz, false, 0.25f);
  return makeEndpointSource(c.mic == "device" ? c.micDevice : std::string(), /*loopback=*/false);
}

}  // namespace vv
