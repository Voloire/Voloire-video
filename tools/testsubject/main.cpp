// TestSubject: a stand-in for a game. Opens a window that flashes once per second while its own
// audio stream beeps in the same instant, so a recording can be checked for A/V sync. The
// "noise" role plays a continuous tone from a different process to prove the SYS channel only
// carries the selected application.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Opts {
  std::string role = "tone";  // tone | noise
  double hz = 440;
  double amp = 0.5;
  bool beepOnFlash = true;
  bool borderless = false;
  bool noWindow = false;
  std::wstring title = L"VV TestSubject";
  double exitAfter = 0;
  std::wstring readyFile;
};

Opts g;
std::atomic<bool> g_run{true};
std::atomic<long long> g_t0{0};
HWND g_hwnd = nullptr;

long long nowUs() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000000LL / f.QuadPart;
}

bool flashAt(long long t) { return t >= 0 && (t % 1000000) < 100000; }

void audioThread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IMMDeviceEnumerator> en;
  ComPtr<IMMDevice> dev;
  ComPtr<IAudioClient> cl;
  ComPtr<IAudioRenderClient> rc;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return;
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(cl.GetAddressOf())))) return;
  WAVEFORMATEX* mix = nullptr;
  cl->GetMixFormat(&mix);
  if (FAILED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr))) return;
  UINT32 bufFrames = 0;
  cl->GetBufferSize(&bufFrames);
  cl->GetService(IID_PPV_ARGS(&rc));
  const int ch = mix->nChannels;
  const double rate = mix->nSamplesPerSec;
  const bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32);
  g_t0 = nowUs();
  cl->Start();
  unsigned long long idx = 0;
  while (g_run) {
    UINT32 pad = 0;
    cl->GetCurrentPadding(&pad);
    UINT32 n = bufFrames - pad;
    if (n > rate / 20) n = (UINT32)(rate / 20);
    if (n) {
      BYTE* data = nullptr;
      if (SUCCEEDED(rc->GetBuffer(n, &data))) {
        for (UINT32 i = 0; i < n; ++i) {
          long long t = (long long)((idx + i) * 1e6 / rate);
          float s = 0.f;
          if (!g.beepOnFlash || flashAt(t)) s = (float)(g.amp * std::sin(2.0 * 3.14159265358979 * g.hz * (idx + i) / rate));
          if (isFloat) for (int c = 0; c < ch; ++c) reinterpret_cast<float*>(data)[i * ch + c] = s;
          else for (int c = 0; c < ch; ++c) reinterpret_cast<int16_t*>(data)[i * ch + c] = (int16_t)(s * 32767);
        }
        rc->ReleaseBuffer(n, 0);
        idx += n;
      }
    }
    Sleep(5);
  }
  cl->Stop();
  CoTaskMemFree(mix);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  static unsigned counter = 0;
  switch (m) {
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      bool flash = g.beepOnFlash && flashAt(nowUs() - g_t0.load());
      HBRUSH bg = CreateSolidBrush(flash ? RGB(255, 255, 255) : RGB(8, 24, 60));
      RECT body{0, 24, rc.right, rc.bottom};
      FillRect(dc, &body, bg);
      DeleteObject(bg);
      HBRUSH on = CreateSolidBrush(RGB(255, 255, 255)), off = CreateSolidBrush(RGB(0, 0, 0));
      for (int b = 0; b < 24; ++b) {
        RECT cell{b * 20, 0, b * 20 + 20, 24};
        FillRect(dc, &cell, ((counter >> (23 - b)) & 1) ? on : off);
      }
      DeleteObject(on);
      DeleteObject(off);
      if (!flash) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 220, 255));
        wchar_t t[128];
        swprintf_s(t, L"%s  %.0f Hz  frame %u", g.title.c_str(), g.hz, counter);
        TextOutW(dc, 16, 60, t, (int)wcslen(t));
        HBRUSH bar = CreateSolidBrush(RGB(0, 240, 255));
        int x = (int)((counter * 6) % (rc.right > 80 ? rc.right - 40 : 40));
        RECT br{x, 120, x + 40, 180};
        FillRect(dc, &br, bar);
        DeleteObject(bar);
      }
      ++counter;
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: g_run = false; PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  for (int i = 1; i < argc; ++i) {
    std::wstring k = argv[i];
    auto val = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (k == L"--role") { std::wstring v = val(); g.role = std::string(v.begin(), v.end()); }
    else if (k == L"--hz") g.hz = _wtof(val().c_str());
    else if (k == L"--amp") g.amp = _wtof(val().c_str());
    else if (k == L"--title") g.title = val();
    else if (k == L"--borderless") g.borderless = true;
    else if (k == L"--exit-after") g.exitAfter = _wtof(val().c_str());
    else if (k == L"--ready-file") g.readyFile = val();
  }
  if (g.role == "noise") { g.beepOnFlash = false; g.noWindow = true; if (g.hz == 440) g.hz = 880; g.amp = 0.3; }

  std::thread at(audioThread);
  if (!g.noWindow) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"VvTestSubject";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    DWORD style = g.borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int w = 1280, h = 720, x = 120, y = 80;
    if (g.borderless) { x = 0; y = 0; w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, g.title.c_str(), style | WS_VISIBLE, x, y, w, h, nullptr, nullptr, hi, nullptr);
    SetTimer(g_hwnd, 1, 15, nullptr);
  }
  if (!g.readyFile.empty()) {
    std::ofstream f(g.readyFile);
    f << "{\"pid\":" << GetCurrentProcessId() << ",\"hwnd\":" << (long long)(intptr_t)g_hwnd << "}";
  }
  long long deadline = g.exitAfter > 0 ? nowUs() + (long long)(g.exitAfter * 1e6) : 0;
  MSG msg;
  while (g_run) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) g_run = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (deadline && nowUs() > deadline) g_run = false;
    Sleep(5);
  }
  g_run = false;
  if (at.joinable()) at.join();
  return 0;
}
