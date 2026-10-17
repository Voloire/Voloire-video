#include "share_copy.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include "encoder.h"
#include "telemetry.h"

namespace vv {

namespace fs = std::filesystem;

namespace {
std::string avErr(int e) {
  char b[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(e, b, sizeof b);
  return b;
}
int evenFloor(int v) { return v - (v & 1); }
}  // namespace

SharePlan planShare(double durationSec, int srcW, int srcH, double srcFps, int targetMB, bool hasAudio) {
  SharePlan p;
  if (durationSec <= 0) durationSec = 0.001;
  if (targetMB <= 0) targetMB = 1;
  p.audioKbps = hasAudio ? 128 : 0;
  p.videoKbps = (int)std::floor((double)targetMB * 8388.608 * 0.92 / durationSec - p.audioKbps);  // 1 MB = 1 MiB
  if (p.videoKbps < 150) {
    const int s = (int)std::floor((double)targetMB * 8388.608 * 0.92 / (150 + p.audioKbps));
    p.error = "too long for " + std::to_string(targetMB) + " MB: at most " + std::to_string(s) + " s";
    return p;
  }
  p.width = std::max(2, evenFloor(srcW));
  p.height = std::max(2, evenFloor(srcH));
  p.fps = (int)std::lround(srcFps);
  if (p.fps <= 0) p.fps = 30;
  auto toHeight = [&](int h) {
    p.width = std::max(2, evenFloor((int)std::lround((double)p.width * h / p.height)));
    p.height = h;
  };
  while (true) {
    const double bpp = (double)p.videoKbps * 1000.0 / ((double)p.width * p.height * p.fps);
    if (bpp >= 0.04) break;
    if (p.fps > 30) { p.fps = 30; continue; }
    if (p.height > 1080) { toHeight(1080); continue; }
    if (p.height > 720) { toHeight(720); continue; }
    if (p.height > 480) { toHeight(480); continue; }
    break;  // 480p30 is the floor, even when bpp stays under 0.04
  }
  return p;
}

json ShareResult::toJson() const {
  return {{"ok", error.empty()}, {"path", path}, {"bytes", bytes}, {"attempts", attempts}, {"error", error}};
}

ShareResult makeShareCopy(const std::string& src, int targetMB, std::function<void(double)> progress,
                          std::atomic<bool>* cancel) {
  ShareResult res;
  AVFormatContext* in = nullptr;
  int r = avformat_open_input(&in, src.c_str(), nullptr, nullptr);
  if (r < 0) {
    res.error = "cannot open " + src + ": " + avErr(r);
    return res;
  }
  auto fail = [&](const std::string& m) {
    res.error = m;
    VV_LOG(Warn, "share", "failed", {{"src", src}, {"error", m}});
    return res;
  };
  r = avformat_find_stream_info(in, nullptr);
  if (r < 0) {
    std::string m = "cannot read " + src + ": " + avErr(r);
    avformat_close_input(&in);
    return fail(m);
  }

  const int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vIdx < 0) {
    avformat_close_input(&in);
    return fail("no video stream in " + src);
  }
  int aIdx = -1;  // the "MIX" track when present, else the first audio stream
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (in->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
    AVDictionaryEntry* t = av_dict_get(in->streams[i]->metadata, "title", nullptr, 0);
    if (t && std::string(t->value) == "MIX") { aIdx = (int)i; break; }
    if (aIdx < 0) aIdx = (int)i;
  }

  AVStream* vst = in->streams[vIdx];
  double durSec = in->duration != AV_NOPTS_VALUE ? (double)in->duration / AV_TIME_BASE : 0;
  if (durSec <= 0 && vst->duration > 0) durSec = (double)vst->duration * av_q2d(vst->time_base);
  if (durSec <= 0) durSec = 1;
  AVRational fr = vst->avg_frame_rate.num ? vst->avg_frame_rate : vst->r_frame_rate;
  double srcFps = av_q2d(fr);
  if (srcFps <= 0) srcFps = 30;

  const AVCodec* vc = avcodec_find_decoder(vst->codecpar->codec_id);
  AVCodecContext* vdec = vc ? avcodec_alloc_context3(vc) : nullptr;
  if (!vdec || avcodec_parameters_to_context(vdec, vst->codecpar) < 0 || avcodec_open2(vdec, vc, nullptr) < 0) {
    if (vdec) avcodec_free_context(&vdec);
    avformat_close_input(&in);
    return fail("no video decoder for " + src);
  }

  AVCodecContext* adec = nullptr;
  SwrContext* swr = nullptr;
  if (aIdx >= 0) {
    AVStream* ast = in->streams[aIdx];
    const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
    if (ac) {
      adec = avcodec_alloc_context3(ac);
      if (adec && (avcodec_parameters_to_context(adec, ast->codecpar) < 0 || avcodec_open2(adec, ac, nullptr) < 0)) {
        avcodec_free_context(&adec);
      }
    }
    if (adec) {
      AVChannelLayout outLayout;
      av_channel_layout_default(&outLayout, kChannels);
      if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, kSampleRate, &adec->ch_layout, adec->sample_fmt,
                              adec->sample_rate, 0, nullptr) < 0 ||
          swr_init(swr) < 0) {
        swr_free(&swr);
      }
    }
  }
  const bool hasAudio = adec && swr;

  auto cleanup = [&] {
    if (swr) swr_free(&swr);
    if (adec) avcodec_free_context(&adec);
    avcodec_free_context(&vdec);
    avformat_close_input(&in);
  };

  SharePlan plan = planShare(durSec, vst->codecpar->width, vst->codecpar->height, srcFps, targetMB, hasAudio);
  if (!plan.error.empty()) {
    cleanup();
    return fail(plan.error);
  }
  VV_LOG(Info, "share", "start",
         {{"src", src}, {"targetMB", targetMB}, {"videoKbps", plan.videoKbps}, {"size", std::to_string(plan.width) + "x" +
          std::to_string(plan.height)}, {"fps", plan.fps}, {"audio", hasAudio}});

  const fs::path srcPath = pathOf(src);
  const fs::path outPath = srcPath.parent_path() / (strOf(srcPath.stem()) + "_share_" + std::to_string(targetMB) + "MB.mp4");
  fs::path partPath = outPath;
  partPath += ".part";
  const uint64_t targetBytes = (uint64_t)targetMB << 20;
  auto report = [&](double v) {
    Telemetry::get().set("share.progress", v * 100.0);
    if (progress) progress(v);
  };

  int attempt = 0;
  while (attempt < 3) {
    EncoderParams ep;
    ep.path = strOf(partPath);
    ep.container = "mp4";
    ep.format = "mp4";
    ep.codec = "h264";
    ep.encoder = "auto";
    ep.width = plan.width;
    ep.height = plan.height;
    ep.fps = plan.fps;
    ep.bitrateKbps = plan.videoKbps;
    ep.rateControl = "cbr";
    ep.gopSeconds = 2;
    ep.audioKbps = 128;
    ep.faststart = true;
    if (hasAudio) ep.audioTracks = {"MIX"};

    Muxer mx;
    std::string err;
    if (!mx.open(ep, err)) {
      std::error_code ec;
      fs::remove(partPath, ec);
      cleanup();
      return fail("share encoder: " + err);
    }
    report(0.0);

    av_seek_frame(in, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vdec);
    if (hasAudio) avcodec_flush_buffers(adec);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    SwsContext* sws = nullptr;
    VideoFrame vf;
    std::vector<uint8_t> abuf;
    int64_t baseUs = INT64_MIN, lastWrittenUs = INT64_MIN;
    const int64_t stepUs = (int64_t)std::llround(1e6 / plan.fps);
    bool aborted = false;
    std::string rerr;

    auto writeVideoFrame = [&](AVFrame* sf) -> bool {
      int64_t ts = sf->best_effort_timestamp != AV_NOPTS_VALUE ? sf->best_effort_timestamp : sf->pts;
      if (ts == AV_NOPTS_VALUE) ts = 0;
      int64_t ptsUs = av_rescale_q(ts, vst->time_base, AVRational{1, 1000000});
      if (baseUs == INT64_MIN) baseUs = ptsUs;
      ptsUs -= baseUs;
      if (ptsUs < 0) ptsUs = 0;
      if (lastWrittenUs != INT64_MIN && ptsUs < lastWrittenUs + (stepUs * 3) / 4) return true;  // fps reduction
      sws = sws_getCachedContext(sws, sf->width, sf->height, (AVPixelFormat)sf->format, plan.width, plan.height,
                                 AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws) { rerr = "scaler init failed"; return false; }
      vf.w = plan.width;
      vf.h = plan.height;
      vf.bgra.resize((size_t)plan.width * plan.height * 4);
      uint8_t* dst[1] = {vf.bgra.data()};
      int dstride[1] = {plan.width * 4};
      sws_scale(sws, sf->data, sf->linesize, 0, sf->height, dst, dstride);
      if (!mx.writeVideo(vf, ptsUs)) { rerr = "video write failed"; return false; }
      lastWrittenUs = ptsUs;
      report(std::min(1.0, (double)ptsUs / (durSec * 1e6)));
      return true;
    };
    auto writeAudioFrame = [&](AVFrame* sf) -> bool {
      const int outMax = swr_get_out_samples(swr, sf->nb_samples);
      if (outMax <= 0) return true;
      abuf.assign((size_t)outMax * kChannels * sizeof(float), 0);
      uint8_t* outp = abuf.data();
      const int got = swr_convert(swr, &outp, outMax, (const uint8_t**)sf->extended_data, sf->nb_samples);
      if (got < 0) { rerr = "resample failed"; return false; }
      if (got > 0 && !mx.writeAudio(0, (const float*)abuf.data(), (size_t)got)) { rerr = "audio write failed"; return false; }
      return true;
    };

    while (!aborted && rerr.empty()) {
      if (cancel && cancel->load()) { aborted = true; break; }
      r = av_read_frame(in, pkt);
      if (r == AVERROR_EOF) break;
      if (r < 0) { rerr = "read failed: " + avErr(r); break; }
      if (pkt->stream_index == vIdx) {
        r = avcodec_send_packet(vdec, pkt);
        av_packet_unref(pkt);
        if (r < 0) { rerr = "video decode failed: " + avErr(r); break; }
        while (true) {
          const int rr = avcodec_receive_frame(vdec, f);
          if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
          if (rr < 0) { rerr = "video decode failed: " + avErr(rr); break; }
          if (!writeVideoFrame(f)) break;
        }
        av_frame_unref(f);
      } else if (hasAudio && pkt->stream_index == aIdx) {
        r = avcodec_send_packet(adec, pkt);
        av_packet_unref(pkt);
        if (r < 0) { rerr = "audio decode failed: " + avErr(r); break; }
        while (true) {
          const int rr = avcodec_receive_frame(adec, f);
          if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
          if (rr < 0) { rerr = "audio decode failed: " + avErr(rr); break; }
          if (!writeAudioFrame(f)) break;
        }
        av_frame_unref(f);
      } else {
        av_packet_unref(pkt);
      }
    }

    if (!aborted && rerr.empty()) {  // drain both decoders, then the resampler
      avcodec_send_packet(vdec, nullptr);
      while (true) {
        const int rr = avcodec_receive_frame(vdec, f);
        if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
        if (rr < 0) { rerr = "video flush failed: " + avErr(rr); break; }
        if (!writeVideoFrame(f)) break;
      }
      av_frame_unref(f);
      if (rerr.empty() && hasAudio) {
        avcodec_send_packet(adec, nullptr);
        while (true) {
          const int rr = avcodec_receive_frame(adec, f);
          if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
          if (rr < 0) { rerr = "audio flush failed: " + avErr(rr); break; }
          if (!writeAudioFrame(f)) break;
        }
        av_frame_unref(f);
        while (rerr.empty()) {
          uint8_t* outp = abuf.data();
          const int got = swr_convert(swr, &outp, (int)(abuf.size() / (kChannels * sizeof(float))), nullptr, 0);
          if (got <= 0) break;
          if (!mx.writeAudio(0, (const float*)abuf.data(), (size_t)got)) { rerr = "audio write failed"; break; }
        }
      }
    }

    av_frame_free(&f);
    av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    std::error_code ec;
    if (aborted || !rerr.empty()) {
      mx.close(err);
      fs::remove(partPath, ec);
      cleanup();
      return fail(aborted ? "cancelled" : rerr);
    }
    if (!mx.close(err)) {
      fs::remove(partPath, ec);
      cleanup();
      return fail("finalise: " + err);
    }
    report(1.0);

    uint64_t bytes = 0;
    if (fs::exists(partPath)) bytes = (uint64_t)fs::file_size(partPath, ec);
    ++attempt;
    if (bytes > 0 && bytes <= targetBytes) {
      fs::remove(outPath, ec);
      fs::rename(partPath, outPath, ec);
      if (ec) {
        fs::remove(partPath, ec);
        cleanup();
        return fail("cannot move the share copy into place: " + ec.message());
      }
      res.path = strOf(outPath);
      res.bytes = bytes;
      res.attempts = attempt;
      VV_LOG(Info, "share", "done", {{"path", res.path}, {"bytes", bytes}, {"attempts", attempt}});
      cleanup();
      return res;
    }
    if (attempt >= 3) {
      fs::remove(partPath, ec);
      cleanup();
      return fail("too big for " + std::to_string(targetMB) + " MB: " + std::to_string(bytes) + " bytes after " +
                  std::to_string(attempt) + " attempts");
    }
    plan.videoKbps = std::max(150, (int)std::floor(plan.videoKbps * 0.85 * (double)targetBytes / (double)std::max<uint64_t>(bytes, 1)));
    VV_LOG(Info, "share", "retry", {{"attempt", attempt}, {"bytes", bytes}, {"videoKbps", plan.videoKbps}});
  }
  cleanup();
  return fail("too big for " + std::to_string(targetMB) + " MB");
}

}  // namespace vv
