#include "wasapi_source.h"
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <chrono>
#include <future>
#include "devices.h"
#include "telemetry.h"

using Microsoft::WRL::ComPtr;

namespace vv {

namespace {

std::string hrText(const char* what, HRESULT hr) {
  char b[160];
  std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, (unsigned long)hr);
  return b;
}

// Completion handler for ActivateAudioInterfaceAsync (free-threaded, signals an event).
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
 public:
  ActivateHandler() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~ActivateHandler() { CloseHandle(done_); }
  HANDLE done() const { return done_; }

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else if (riid == __uuidof(IAgileObject)) {
      *out = static_cast<IAgileObject*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
    SetEvent(done_);
    return S_OK;
  }

 private:
  std::atomic<ULONG> ref_{1};
  HANDLE done_;
};

// Base: owns the capture thread; subclasses supply an initialised IAudioClient and its format.
class WasapiCapture : public IAudioSource {
 public:
  ~WasapiCapture() override { stop(); }

  bool start(std::string& err) override {
    std::promise<std::string> ready;
    auto fut = ready.get_future();
    run_ = true;
    th_ = std::thread([this, p = std::move(ready)]() mutable { loop(std::move(p)); });
    if (fut.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
      err = name() + ": audio client did not start in time";
      stop();
      return false;
    }
    err = fut.get();
    if (!err.empty()) {
      stop();
      return false;
    }
    return true;
  }

  void stop() override {
    run_ = false;
    if (th_.joinable()) th_.join();
  }

 protected:
  // Runs on the capture thread (MTA). Must leave the client initialised for event callbacks.
  virtual bool open(ComPtr<IAudioClient>& client, WAVEFORMATEX& fmt, std::string& err) = 0;

 private:
  void loop(std::promise<std::string> ready) {
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IAudioClient> client;
    WAVEFORMATEX fmt{};
    std::string err;
    ComPtr<IAudioCaptureClient> cap;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = open(client, fmt, err);
    if (ok) {
      HRESULT hr = client->SetEventHandle(ev);
      if (FAILED(hr)) { err = hrText("SetEventHandle", hr); ok = false; }
      else if (FAILED(hr = client->GetService(IID_PPV_ARGS(&cap)))) { err = hrText("GetService(capture)", hr); ok = false; }
      else if (FAILED(hr = client->Start())) { err = hrText("IAudioClient::Start", hr); ok = false; }
    }
    ready.set_value(ok ? std::string() : err);
    if (!ok) {
      CloseHandle(ev);
      if (SUCCEEDED(cohr)) CoUninitialize();
      return;
    }
    const bool isFloat = fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    std::vector<float> tmp;
    while (run_) {
      WaitForSingleObject(ev, 40);
      for (;;) {
        UINT32 pkt = 0;
        if (FAILED(cap->GetNextPacketSize(&pkt)) || pkt == 0) break;
        BYTE* data = nullptr;
        UINT32 n = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
        tmp.assign((size_t)n * kChannels, 0.f);
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
          if (isFloat) std::memcpy(tmp.data(), data, tmp.size() * sizeof(float));
          else {
            auto* s = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < tmp.size(); ++i) tmp[i] = s[i] / 32768.0f;
          }
        }
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) Telemetry::get().inc("audio.discontinuities");
        cap->ReleaseBuffer(n);
        ring_.push(tmp.data(), n);
      }
    }
    client->Stop();
    CloseHandle(ev);
    if (SUCCEEDED(cohr)) CoUninitialize();
  }

  std::atomic<bool> run_{false};
  std::thread th_;
};

class ProcessLoopback : public WasapiCapture {
 public:
  explicit ProcessLoopback(uint32_t pid) : pid_(pid) {}
  std::string name() const override { return "app audio (pid " + std::to_string(pid_) + ")"; }

 protected:
  bool open(ComPtr<IAudioClient>& client, WAVEFORMATEX& fmt, std::string& err) override {
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid_;
    ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof ap;
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);

    auto* h = new ActivateHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h, &op);
    if (FAILED(hr)) { h->Release(); err = hrText("ActivateAudioInterfaceAsync", hr); return false; }
    DWORD w = WaitForSingleObject(h->done(), 5000);
    h->Release();
    if (w != WAIT_OBJECT_0) { err = "process loopback activation timed out"; return false; }
    HRESULT act = E_FAIL;
    ComPtr<IUnknown> unk;
    hr = op->GetActivateResult(&act, &unk);
    if (FAILED(hr) || FAILED(act)) { err = hrText("process loopback activation", FAILED(hr) ? hr : act); return false; }
    hr = unk.As(&client);
    if (FAILED(hr)) { err = hrText("IAudioClient from activation", hr); return false; }

    fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = kChannels;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            200000, 0, &fmt, nullptr);
    if (FAILED(hr)) { err = hrText("process loopback Initialize", hr); return false; }
    return true;
  }

 private:
  uint32_t pid_;
};

class EndpointCapture : public WasapiCapture {
 public:
  EndpointCapture(std::string id, bool loopback) : id_(std::move(id)), loopback_(loopback) {}
  std::string name() const override {
    return std::string(loopback_ ? "system output" : "microphone") + (id_.empty() ? " (default)" : "");
  }

 protected:
  bool open(ComPtr<IAudioClient>& client, WAVEFORMATEX& fmt, std::string& err) override {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) { err = hrText("MMDeviceEnumerator", hr); return false; }
    ComPtr<IMMDevice> dev;
    if (id_.empty()) hr = en->GetDefaultAudioEndpoint(loopback_ ? eRender : eCapture, eConsole, &dev);
    else hr = en->GetDevice(wide(id_).c_str(), &dev);
    if (FAILED(hr)) { err = hrText(id_.empty() ? "default audio endpoint" : "audio device lookup", hr); return false; }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) { err = hrText("IAudioClient activate", hr); return false; }

    fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = kChannels;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = fmt.nChannels * 4;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | (loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &fmt, nullptr);
    if (FAILED(hr)) { err = hrText("endpoint Initialize", hr); return false; }
    return true;
  }

 private:
  std::string id_;
  bool loopback_;
};

}  // namespace

std::unique_ptr<IAudioSource> makeProcessLoopbackSource(uint32_t pid) { return std::make_unique<ProcessLoopback>(pid); }

std::unique_ptr<IAudioSource> makeEndpointSource(const std::string& deviceId, bool loopback) {
  return std::make_unique<EndpointCapture>(deviceId, loopback);
}

}  // namespace vv
