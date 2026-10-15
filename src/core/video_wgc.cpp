// Windows Graphics Capture source (C++/WinRT). All WinRT objects live on one dedicated MTA thread.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <future>
#include <mutex>
#include "telemetry.h"
#include "video_sources.h"

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace d3dwinrt = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace vv {

namespace {

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) VvDxgiAccess : ::IUnknown {
  virtual HRESULT __stdcall GetInterface(REFIID iid, void** object) = 0;
};

struct MonList { std::vector<HMONITOR> m; };
BOOL CALLBACK monCb(HMONITOR m, HDC, LPRECT, LPARAM lp) {
  reinterpret_cast<MonList*>(lp)->m.push_back(m);
  return TRUE;
}

class WgcSource : public IVideoSource {
 public:
  WgcSource(HWND hwnd, int monitor) : hwnd_(hwnd), monitor_(monitor) {}
  ~WgcSource() override { stop(); }

  bool start(std::function<void(FramePtr)> sink, std::function<void(const std::string&)> onClosed,
             std::string& err) override {
    // WinRT worker threads live in the capture DLLs. When our capture thread ends and its
    // apartment tears down, COM would unload those DLLs under the workers (crash on stop).
    // Keep the MTA and the modules alive for the life of the process.
    static std::once_flag once;
    std::call_once(once, [] {
      CO_MTA_USAGE_COOKIE cookie{};
      CoIncrementMTAUsage(&cookie);
    });
    sink_ = std::move(sink);
    onClosed_ = std::move(onClosed);
    std::promise<std::string> ready;
    auto fut = ready.get_future();
    run_ = true;
    th_ = std::thread([this, p = std::move(ready)]() mutable { run(std::move(p)); });
    if (fut.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
      err = "capture did not start in time";
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

  std::string name() const override { return hwnd_ ? "WGC window" : "WGC monitor " + std::to_string(monitor_); }

 private:
  void run(std::promise<std::string> ready) {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
      if (FAILED(hr)) throw winrt::hresult_error(hr, L"D3D11CreateDevice");
      ComPtr<IDXGIDevice> dxgi;
      dev_.As(&dxgi);
      winrt::com_ptr<::IInspectable> insp;
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()));
      d3dDev_ = insp.as<d3dwinrt::IDirect3DDevice>();

      auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      wgc::GraphicsCaptureItem item{nullptr};
      if (hwnd_) {
        winrt::check_hresult(interop->CreateForWindow(hwnd_, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                      winrt::put_abi(item)));
      } else {
        MonList ml;
        EnumDisplayMonitors(nullptr, nullptr, monCb, (LPARAM)&ml);
        if (monitor_ < 0 || monitor_ >= (int)ml.m.size()) throw winrt::hresult_error(E_INVALIDARG, L"monitor index");
        winrt::check_hresult(interop->CreateForMonitor(ml.m[monitor_], winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                       winrt::put_abi(item)));
      }
      lastSize_ = item.Size();
      pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
          d3dDev_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, lastSize_);
      arrived_ = pool_.FrameArrived(winrt::auto_revoke, [this](auto&&, auto&&) { onFrame(); });
      closed_ = item.Closed(winrt::auto_revoke, [this](auto&&, auto&&) {
        if (!closedFired_.exchange(true) && onClosed_) onClosed_("capture target closed");
      });
      session_ = pool_.CreateCaptureSession(item);
      session_.IsCursorCaptureEnabled(true);
      session_.StartCapture();
      for (const wchar_t* dll : {L"Windows.Graphics.Capture.dll", L"Windows.Graphics.dll", L"Windows.UI.dll"}) {
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, dll, &pinned);
      }
      { std::lock_guard lk(ctxMu_); active_ = true; }
      ready.set_value({});
    } catch (const winrt::hresult_error& e) {
      ready.set_value("Windows Graphics Capture: " + winrt::to_string(e.message()));
      return;
    } catch (const std::exception& e) {
      ready.set_value(std::string("Windows Graphics Capture: ") + e.what());
      return;
    }
    while (run_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      // item.Closed does not always fire when the process just exits: watch the handle too.
      if (hwnd_ && !IsWindow(hwnd_) && !closedFired_.exchange(true) && onClosed_) onClosed_("capture target closed");
    }
    { std::lock_guard lk(ctxMu_); active_ = false; }  // in-flight callbacks finish, later ones bail out
    try {
      arrived_.revoke();
      closed_.revoke();
      if (session_) session_.Close();
      if (pool_) pool_.Close();
    } catch (...) {}
    std::lock_guard lk(ctxMu_);
    session_ = nullptr;
    pool_ = nullptr;
    d3dDev_ = nullptr;
    staging_.Reset();
    ctx_.Reset();
    dev_.Reset();
  }

  void onFrame() {
    std::lock_guard lk(ctxMu_);
    if (!active_) return;
    try {
      auto frame = pool_.TryGetNextFrame();
      if (!frame) return;
      auto content = frame.ContentSize();
      winrt::com_ptr<VvDxgiAccess> access;
      winrt::check_hresult(winrt::get_unknown(frame.Surface())->QueryInterface(__uuidof(VvDxgiAccess), access.put_void()));
      ComPtr<ID3D11Texture2D> tex;
      winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&tex)));
      D3D11_TEXTURE2D_DESC d;
      tex->GetDesc(&d);

      if (!staging_ || stagingW_ != d.Width || stagingH_ != d.Height) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        staging_.Reset();
        winrt::check_hresult(dev_->CreateTexture2D(&sd, nullptr, &staging_));
        stagingW_ = d.Width;
        stagingH_ = d.Height;
      }
      ctx_->CopyResource(staging_.Get(), tex.Get());
      D3D11_MAPPED_SUBRESOURCE m{};
      winrt::check_hresult(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m));
      int w = std::min<int>(content.Width, d.Width) & ~1;
      int h = std::min<int>(content.Height, d.Height) & ~1;
      if (w > 0 && h > 0) {
        auto f = std::make_shared<VideoFrame>();
        f->w = w; f->h = h; f->capUs = nowUs(); f->seq = seq_++;
        f->bgra.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
          std::memcpy(f->bgra.data() + (size_t)y * w * 4, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        ctx_->Unmap(staging_.Get(), 0);
        sink_(std::move(f));
      } else {
        ctx_->Unmap(staging_.Get(), 0);
      }
      if (content.Width != lastSize_.Width || content.Height != lastSize_.Height) {
        lastSize_ = content;
        pool_.Recreate(d3dDev_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
        VV_LOG(Info, "capture", "capture target resized", {{"w", content.Width}, {"h", content.Height}});
      }
    } catch (...) {
      Telemetry::get().inc("video.capture_errors");
    }
  }

  HWND hwnd_;
  int monitor_;
  std::function<void(FramePtr)> sink_;
  std::function<void(const std::string&)> onClosed_;
  std::atomic<bool> run_{false}, closedFired_{false};
  std::thread th_;
  std::mutex ctxMu_;
  bool active_ = false;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<ID3D11Texture2D> staging_;
  UINT stagingW_ = 0, stagingH_ = 0;
  d3dwinrt::IDirect3DDevice d3dDev_{nullptr};
  wgc::Direct3D11CaptureFramePool pool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived_;
  wgc::GraphicsCaptureItem::Closed_revoker closed_;
  winrt::Windows::Graphics::SizeInt32 lastSize_{};
  uint64_t seq_ = 0;
};

}  // namespace

std::unique_ptr<IVideoSource> makeWgcWindowSource(void* hwnd) {
  return std::make_unique<WgcSource>(reinterpret_cast<HWND>(hwnd), 0);
}
std::unique_ptr<IVideoSource> makeWgcMonitorSource(int monitorIndex) {
  return std::make_unique<WgcSource>(nullptr, monitorIndex);
}

}  // namespace vv
