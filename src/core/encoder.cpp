#include "encoder.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include "common.h"
#include "redact.h"
#include "telemetry.h"

namespace vv {

namespace {
std::atomic<bool> g_probing{false};

void avLogToTelemetry(void*, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_WARNING) return;
  char line[512];
  vsnprintf(line, sizeof line, fmt, vl);
  std::string m = line;
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (m.empty()) return;
  // While candidates are tried in turn, a missing GPU or driver is expected, not an error.
  Telemetry::get().log(g_probing ? Level::Info : level <= AV_LOG_ERROR ? Level::Error : Level::Warn, "ffmpeg", m);
}

std::string avErr(int e) {
  char b[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(e, b, sizeof b);
  return b;
}
}  // namespace

struct AudioTrack {
  std::string name;
  AVStream* st = nullptr;
  AVCodecContext* ctx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* pkt = nullptr;     // own packet: tracks encode on different threads
  std::vector<float> pending;  // interleaved
  int64_t samples = 0;         // encoded so far, also the next pts
};

struct Muxer::Impl {
  EncoderParams p;
  AVFormatContext* fmt = nullptr;
  AVStream* vst = nullptr;
  AVCodecContext* vctx = nullptr;
  AVFrame* vframe = nullptr;
  AVPacket* pkt = nullptr;     // video packet (the video thread only)
  SwsContext* sws = nullptr;
  int inW = 0, inH = 0;
  int64_t lastVpts = -1;
  std::string vname;
  std::vector<AudioTrack> tracks;
  std::mutex writeMu;
  std::atomic<uint64_t> vframes{0};
  bool headerWritten = false;

  // Network/async mode: packets queue to a writer thread, so capture and encoding never block
  // on the socket. A stalled peer is cut loose whether or not the OS timeout fires, by the same
  // deadline watched from the AVIO interrupt callback.
  bool network = false;
  bool async = false;
  int maxQueueMs = 3000;
  struct QItem {
    AVPacket* pkt;
    int64_t ptsUs;
    bool video;
  };
  std::deque<QItem> queue;
  std::mutex qMu;
  std::condition_variable qCv;
  std::thread writer;
  std::atomic<bool> closing{false};
  std::atomic<bool> failedFlag{false};
  std::atomic<uint64_t> dropped{0};
  std::atomic<int64_t> lastEnqV{-1}, lastSentV{-1};
  std::atomic<int64_t> lastIoUs{0};
  bool dropMode = false;  // guarded by qMu
  AVIOInterruptCB cb{};

  ~Impl() { release(); }

  static int interruptCb(void* o) {
    auto* d = (Impl*)o;
    if (!d->network) return 0;
    return nowUs() - d->lastIoUs.load(std::memory_order_relaxed) > 5000000 ? 1 : 0;
  }

  void enqueue(AVPacket* pkt, bool video, int64_t ptsUs) {
    std::lock_guard lk(qMu);
    if (failedFlag.load()) {  // the peer is gone: free it here, the queue must not grow
      av_packet_free(&pkt);
      ++dropped;
      return;
    }
    if (video) {
      lastEnqV = ptsUs;
      int64_t sent = lastSentV.load();
      if (!dropMode && sent >= 0 && ptsUs - sent > (int64_t)maxQueueMs * 1000) dropMode = true;
      if (dropMode) {
        if (pkt->flags & AV_PKT_FLAG_KEY) dropMode = false;  // resume on the next keyframe
        else {
          av_packet_free(&pkt);
          ++dropped;
          return;
        }
      }
    }
    queue.push_back({pkt, ptsUs, video});
    qCv.notify_one();
  }

  void writerLoop() {
    for (;;) {
      QItem q;
      {
        std::unique_lock lk(qMu);
        qCv.wait(lk, [this] { return !queue.empty() || closing.load(); });
        if (queue.empty()) return;  // closing and drained
        q = queue.front();
        queue.pop_front();
      }
      if (failedFlag.load()) {
        av_packet_free(&q.pkt);
        continue;
      }
      int r = av_interleaved_write_frame(fmt, q.pkt);
      if (q.video) lastSentV = q.ptsUs;
      av_packet_free(&q.pkt);
      if (r < 0) failedFlag = true;
      else lastIoUs = nowUs();
    }
  }

  void release() {
    if (writer.joinable()) {
      {
        std::lock_guard lk(qMu);
        closing = true;
      }
      qCv.notify_all();
      writer.join();
    }
    {
      std::lock_guard lk(qMu);
      for (auto& q : queue) av_packet_free(&q.pkt);
      queue.clear();
    }
    if (sws) { sws_freeContext(sws); sws = nullptr; }
    if (vframe) av_frame_free(&vframe);
    if (vctx) avcodec_free_context(&vctx);
    for (auto& t : tracks) {
      if (t.frame) av_frame_free(&t.frame);
      if (t.ctx) avcodec_free_context(&t.ctx);
      if (t.pkt) av_packet_free(&t.pkt);
    }
    if (pkt) av_packet_free(&pkt);
    if (fmt) {
      if (fmt->pb && !(fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt->pb);
      avformat_free_context(fmt);
      fmt = nullptr;
    }
  }

  bool openVideoCodec(const char* name, std::string& err) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) { err = std::string(name) + " not available"; return false; }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = p.width;
    c->height = p.height;
    c->time_base = {1, p.fps};
    c->framerate = {p.fps, 1};
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->bit_rate = (int64_t)p.bitrateKbps * 1000;
    if (p.rateControl == "cbr") {
      c->rc_max_rate = c->bit_rate;
      c->rc_buffer_size = p.bitrateKbps * 1000;
    }
    c->gop_size = p.fps * std::max(1, p.gopSeconds);
    c->max_b_frames = 0;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    std::string n = name;
    const char* rc = p.rateControl == "cbr" ? "cbr" : "vbr";
    if (n.find("nvenc") != std::string::npos) {
      av_opt_set(c->priv_data, "preset", "p4", 0);
      av_opt_set(c->priv_data, "tune", "hq", 0);
      av_opt_set(c->priv_data, "rc", rc, 0);
    } else if (n.find("amf") != std::string::npos) {
      av_opt_set(c->priv_data, "quality", "balanced", 0);
      av_opt_set(c->priv_data, "rc", p.rateControl == "cbr" ? "cbr" : "vbr_peak", 0);
    } else if (n.find("qsv") != std::string::npos) {
      av_opt_set(c->priv_data, "preset", "medium", 0);
    }
    int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
      err = std::string(name) + ": " + avErr(r);
      avcodec_free_context(&c);
      return false;
    }
    vctx = c;
    vname = name;
    return true;
  }

  bool openVideo(std::string& err) {
    vst = avformat_new_stream(fmt, nullptr);
    if (!vst) { err = "cannot create video stream"; return false; }
    std::vector<std::string> candidates;
    if (p.encoder == "auto") {
      for (auto s : {"nvenc", "amf", "qsv", "mf"}) candidates.push_back(p.codec + "_" + s);
    } else {
      candidates.push_back(p.codec + "_" + p.encoder);
    }
    std::string why;
    g_probing = true;
    for (auto& c : candidates) {
      std::string e;
      if (openVideoCodec(c.c_str(), e)) { VV_LOG(Info, "encoder", "video encoder opened", {{"name", c}}); break; }
      why += (why.empty() ? "" : "; ") + e;
      VV_LOG(Info, "encoder", "video encoder unavailable", {{"name", c}, {"why", e}});
    }
    g_probing = false;
    if (!vctx) { err = "no video encoder could be opened: " + why; return false; }
    vst->time_base = vctx->time_base;
    avcodec_parameters_from_context(vst->codecpar, vctx);
    vframe = av_frame_alloc();
    vframe->format = AV_PIX_FMT_NV12;
    vframe->width = p.width;
    vframe->height = p.height;
    if (av_frame_get_buffer(vframe, 32) < 0) { err = "video frame alloc failed"; return false; }
    return true;
  }

  bool openAudio(const std::string& name, std::string& err) {
    const AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec) { err = "aac encoder not available"; return false; }
    AudioTrack t;
    t.name = name;
    t.st = avformat_new_stream(fmt, nullptr);
    t.st->disposition = tracks.empty() ? AV_DISPOSITION_DEFAULT : 0;  // players pick the first track
    t.ctx = avcodec_alloc_context3(codec);
    t.ctx->sample_rate = kSampleRate;
    t.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&t.ctx->ch_layout, kChannels);
    t.ctx->bit_rate = (int64_t)std::max(1, p.audioKbps) * 1000;
    t.ctx->time_base = {1, kSampleRate};
    t.samples = (int64_t)(p.startPtsUs * (int64_t)kSampleRate / 1000000);  // 0 except after a reconnect
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) t.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(t.ctx, codec, nullptr);
    if (r < 0) { err = "aac open: " + avErr(r); return false; }
    t.st->time_base = t.ctx->time_base;
    avcodec_parameters_from_context(t.st->codecpar, t.ctx);
    av_dict_set(&t.st->metadata, "title", name.c_str(), 0);
    t.pkt = av_packet_alloc();
    t.frame = av_frame_alloc();
    t.frame->format = AV_SAMPLE_FMT_FLTP;
    t.frame->sample_rate = kSampleRate;
    t.frame->nb_samples = t.ctx->frame_size;
    av_channel_layout_copy(&t.frame->ch_layout, &t.ctx->ch_layout);
    if (av_frame_get_buffer(t.frame, 0) < 0) { err = "audio frame alloc failed"; return false; }
    tracks.push_back(std::move(t));
    return true;
  }

  bool writePackets(AVCodecContext* c, AVStream* st, AVPacket* pkt) {
    for (;;) {
      int r = avcodec_receive_packet(c, pkt);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      av_packet_rescale_ts(pkt, c->time_base, st->time_base);
      pkt->stream_index = st->index;
      if (async) {
        int64_t us = pkt->pts == AV_NOPTS_VALUE ? -1 : (int64_t)std::llround(pkt->pts * av_q2d(st->time_base) * 1e6);
        AVPacket* cl = av_packet_clone(pkt);
        bool video = st == vst;
        av_packet_unref(pkt);
        if (!cl) {
          failedFlag = true;
          return false;
        }
        enqueue(cl, video, us);
      } else {
        {
          std::lock_guard lk(writeMu);
          r = av_interleaved_write_frame(fmt, pkt);
        }
        av_packet_unref(pkt);
        lastIoUs = nowUs();
        if (r < 0) {
          failedFlag = true;
          return false;
        }
      }
    }
  }

  bool encodeAudioFrame(AudioTrack& t, const float* interleaved, int n, bool flushPad) {
    av_frame_make_writable(t.frame);
    auto* l = reinterpret_cast<float*>(t.frame->extended_data[0]);
    auto* rr = reinterpret_cast<float*>(t.frame->extended_data[1]);
    for (int i = 0; i < t.ctx->frame_size; ++i) {
      bool have = i < n;
      l[i] = have ? interleaved[i * 2] : 0.f;
      rr[i] = have ? interleaved[i * 2 + 1] : 0.f;
    }
    (void)flushPad;
    t.frame->pts = t.samples;
    t.samples += t.ctx->frame_size;
    if (avcodec_send_frame(t.ctx, t.frame) < 0) return false;
    return writePackets(t.ctx, t.st, t.pkt);
  }
};

Muxer::Muxer() : d_(std::make_unique<Impl>()) {}
Muxer::~Muxer() = default;

bool Muxer::open(const EncoderParams& p, std::string& err) {
  auto& d = *d_;
  d.p = p;
  d.network = p.path.rfind("rtmp://", 0) == 0 || p.path.rfind("rtmps://", 0) == 0;
  d.async = p.asyncWrite;
  d.maxQueueMs = p.maxQueueMs;
  if (d.network) {
    static std::once_flag netOnce;
    std::call_once(netOnce, [] { avformat_network_init(); });
  }
  av_log_set_callback(avLogToTelemetry);
  int r = avformat_alloc_output_context2(&d.fmt, nullptr, p.format.empty() ? nullptr : p.format.c_str(), p.path.c_str());
  if (r < 0 || !d.fmt) { err = "cannot create output: " + avErr(r); return false; }
  if (!d.openVideo(err)) return false;
  for (auto& n : p.audioTracks)
    if (!d.openAudio(n, err)) return false;
  if (!(d.fmt->oformat->flags & AVFMT_NOFILE)) {
    if (d.network) {
      // A stalled peer must fail in seconds, not freeze the recording: rw_timeout bounds each
      // blocked call, and the same 5 s deadline is watched from the interrupt callback.
      d.cb = {Impl::interruptCb, &d};
      d.lastIoUs = nowUs();
      AVDictionary* opts = nullptr;
      av_dict_set(&opts, "rw_timeout", "5000000", 0);
      av_dict_set(&opts, "tcp_nodelay", "1", 0);
      r = avio_open2(&d.fmt->pb, p.path.c_str(), AVIO_FLAG_WRITE, &d.cb, &opts);
      av_dict_free(&opts);
    } else {
      r = avio_open(&d.fmt->pb, p.path.c_str(), AVIO_FLAG_WRITE);
    }
    if (r < 0) { err = "cannot open " + redactUrl(p.path) + ": " + avErr(r); return false; }
  }
  AVDictionary* opts = nullptr;
  if (p.container == "mp4")
    av_dict_set(&opts, "movflags", p.faststart ? "+faststart" : "+frag_keyframe+empty_moov+default_base_moof", 0);
  d.lastIoUs = nowUs();
  r = avformat_write_header(d.fmt, &opts);
  av_dict_free(&opts);
  if (r < 0) { err = "write header: " + avErr(r); return false; }
  d.headerWritten = true;
  d.lastIoUs = nowUs();
  d.pkt = av_packet_alloc();
  if (d.async) d.writer = std::thread([&d] { d.writerLoop(); });
  return true;
}

bool Muxer::writeVideo(const VideoFrame& f, int64_t ptsUs) {
  auto& d = *d_;
  d.sws = sws_getCachedContext(d.sws, f.w, f.h, AV_PIX_FMT_BGRA, d.p.width, d.p.height, AV_PIX_FMT_NV12,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!d.sws) return false;
  if (f.w != d.inW || f.h != d.inH) {
    d.inW = f.w;
    d.inH = f.h;
    const int* cs = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(d.sws, cs, 1, cs, 0, 0, 1 << 16, 1 << 16);
  }
  if (av_frame_make_writable(d.vframe) < 0) return false;
  const uint8_t* src[1] = {f.bgra.data()};
  int stride[1] = {f.w * 4};
  sws_scale(d.sws, src, stride, 0, f.h, d.vframe->data, d.vframe->linesize);
  int64_t pts = (int64_t)std::llround((double)ptsUs * d.p.fps / 1e6);
  if (pts <= d.lastVpts) pts = d.lastVpts + 1;
  d.lastVpts = pts;
  d.vframe->pts = pts;
  d.vframe->color_range = AVCOL_RANGE_MPEG;
  d.vframe->colorspace = AVCOL_SPC_BT709;
  int r = avcodec_send_frame(d.vctx, d.vframe);
  if (r < 0) return false;
  ++d.vframes;
  return d.writePackets(d.vctx, d.vst, d.pkt);
}

bool Muxer::writeAudio(int track, const float* in, size_t frames) {
  auto& d = *d_;
  if (track < 0 || track >= (int)d.tracks.size()) return false;
  auto& t = d.tracks[track];
  t.pending.insert(t.pending.end(), in, in + frames * kChannels);
  const size_t fs = (size_t)t.ctx->frame_size;
  size_t off = 0;
  bool ok = true;
  while ((t.pending.size() - off) / kChannels >= fs) {
    ok = d.encodeAudioFrame(t, t.pending.data() + off, (int)fs, false) && ok;
    off += fs * kChannels;
  }
  if (off) t.pending.erase(t.pending.begin(), t.pending.begin() + off);
  return ok;
}

bool Muxer::close(std::string& err) {
  auto& d = *d_;
  if (!d.fmt) return true;
  bool ok = true;
  if (d.vctx) {
    avcodec_send_frame(d.vctx, nullptr);
    ok = d.writePackets(d.vctx, d.vst, d.pkt) && ok;
  }
  for (auto& t : d.tracks) {
    if (!t.pending.empty()) d.encodeAudioFrame(t, t.pending.data(), (int)(t.pending.size() / kChannels), true);
    t.pending.clear();
    avcodec_send_frame(t.ctx, nullptr);
    ok = d.writePackets(t.ctx, t.st, t.pkt) && ok;
  }
  if (d.async && d.writer.joinable()) {  // let the tail packets out, then stop the writer
    d.closing = true;
    d.qCv.notify_all();
    d.writer.join();
  }
  if (d.headerWritten) {
    int r = av_write_trailer(d.fmt);
    if (r < 0) { err = "write trailer: " + avErr(r); ok = false; }
    d.headerWritten = false;
  }
  d.release();
  return ok;
}

std::string Muxer::videoEncoderName() const { return d_->vname; }
uint64_t Muxer::videoFramesWritten() const { return d_->vframes; }
uint64_t Muxer::bytesWritten() const {
  return d_->fmt && d_->fmt->pb ? (uint64_t)avio_tell(d_->fmt->pb) : 0;
}
int Muxer::width() const { return d_->p.width; }
int Muxer::height() const { return d_->p.height; }

bool Muxer::failed() const { return d_->failedFlag.load(); }

double Muxer::queuedMs() const {
  auto& d = *d_;
  int64_t enq = d.lastEnqV.load(), sent = d.lastSentV.load();
  if (enq < 0 || sent < 0) return 0;
  return (double)std::max<int64_t>(0, enq - sent) / 1000.0;
}

uint64_t Muxer::droppedPackets() const { return d_->dropped.load(); }

}  // namespace vv
