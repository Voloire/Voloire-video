// DXGI Desktop Duplication fallback, for exclusive fullscreen where Windows Graphics Capture has limits.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <chrono>
#include <future>
#include "telemetry.h"
#include "video_sources.h"

using Microsoft::WRL::ComPtr;

namespace vv {

namespace {

class DxgiSource : public IVideoSource {
 public:
  explicit DxgiSource(int monitor) : monitor_(monitor) {}
  ~DxgiSource() override { stop(); }

  bool start(std::function<void(FramePtr)> sink, std::function<void(const std::string&)>, std::string& err) override {
    sink_ = std::move(sink);
    std::promise<std::string> ready;
    auto fut = ready.get_future();
    run_ = true;
    th_ = std::thread([this, p = std::move(ready)]() mutable { run(std::move(p)); });
    if (fut.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
      err = "desktop duplication did not start in time";
      stop();
      return false;
    }
    err = fut.get();
    if (!err.empty()) { stop(); return false; }
    return true;
  }

  void stop() override {
    run_ = false;
    if (th_.joinable()) th_.join();
  }
  std::string name() const override { return "DXGI duplication, monitor " + std::to_string(monitor_); }

 private:
  bool open(std::string& err) {
    dup_.Reset();
    ComPtr<IDXGIFactory1> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { err = "CreateDXGIFactory1 failed"; return false; }
    int idx = 0;
    for (UINT a = 0;; ++a) {
      ComPtr<IDXGIAdapter1> ad;
      if (fac->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
      for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> out;
        if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
        if (idx++ != monitor_) continue;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<ID3D11Device> dev;
        HRESULT hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &dev, nullptr, &ctx);
        if (FAILED(hr)) { err = "D3D11CreateDevice failed"; return false; }
        ComPtr<IDXGIOutput1> out1;
        out.As(&out1);
        hr = out1->DuplicateOutput(dev.Get(), &dup_);
        if (FAILED(hr)) { err = "DuplicateOutput failed (0x" + std::to_string((unsigned long)hr) + ")"; return false; }
        dev_ = dev;
        ctx_ = ctx;
        staging_.Reset();
        return true;
      }
    }
    err = "monitor index not found";
    return false;
  }

  void run(std::promise<std::string> ready) {
    std::string err;
    bool ok = open(err);
    ready.set_value(ok ? std::string() : err);
    if (!ok) return;
    uint64_t seq = 0;
    while (run_) {
      DXGI_OUTDUPL_FRAME_INFO info{};
      ComPtr<IDXGIResource> res;
      HRESULT hr = dup_->AcquireNextFrame(100, &info, &res);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
      if (hr == DXGI_ERROR_ACCESS_LOST) {
        Telemetry::get().inc("video.dxgi_access_lost");
        std::string e;
        if (!open(e)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (FAILED(hr)) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
      ComPtr<ID3D11Texture2D> tex;
      res.As(&tex);
      D3D11_TEXTURE2D_DESC d;
      tex->GetDesc(&d);
      if (!staging_) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        dev_->CreateTexture2D(&sd, nullptr, &staging_);
      }
      if (staging_) {
        ctx_->CopyResource(staging_.Get(), tex.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
          int w = (int)d.Width & ~1, h = (int)d.Height & ~1;
          auto f = std::make_shared<VideoFrame>();
          f->w = w; f->h = h; f->capUs = nowUs(); f->seq = seq++;
          f->bgra.resize((size_t)w * h * 4);
          for (int y = 0; y < h; ++y)
            std::memcpy(f->bgra.data() + (size_t)y * w * 4, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
          ctx_->Unmap(staging_.Get(), 0);
          sink_(std::move(f));
        }
      }
      dup_->ReleaseFrame();
    }
  }

  int monitor_;
  std::function<void(FramePtr)> sink_;
  std::atomic<bool> run_{false};
  std::thread th_;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGIOutputDuplication> dup_;
  ComPtr<ID3D11Texture2D> staging_;
};

}  // namespace

std::unique_ptr<IVideoSource> makeDxgiSource(int monitorIndex) { return std::make_unique<DxgiSource>(monitorIndex); }

}  // namespace vv
