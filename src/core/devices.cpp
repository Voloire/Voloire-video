#include "devices.h"
#include <windows.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace vv {

json WindowInfo::toJson() const {
  return {{"hwnd", hwnd}, {"pid", pid}, {"title", title}, {"exe", exe},
          {"w", w},       {"h", h},     {"fullscreen", fullscreen}, {"foreground", foreground}};
}
json AudioDevice::toJson() const {
  return {{"id", id}, {"name", name}, {"capture", capture}, {"default", isDefault}};
}
json MonitorInfo::toJson() const {
  return {{"index", index}, {"name", name}, {"w", w}, {"h", h}, {"primary", primary}};
}

namespace {

std::string exeOf(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};
  wchar_t buf[MAX_PATH * 2];
  DWORD n = (DWORD)std::size(buf);
  std::string r;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
    std::wstring p(buf, n);
    auto s = p.find_last_of(L'\\');
    r = utf8(s == std::wstring::npos ? p : p.substr(s + 1));
  }
  CloseHandle(h);
  return r;
}

bool fillInfo(HWND h, WindowInfo& w) {
  if (!IsWindowVisible(h) || IsIconic(h)) return false;
  int cloaked = 0;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return false;
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
  wchar_t title[512];
  int tl = GetWindowTextW(h, title, (int)std::size(title));
  if (tl <= 0) return false;
  RECT r;
  if (!GetWindowRect(h, &r)) return false;
  w.hwnd = (int64_t)(intptr_t)h;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  w.pid = pid;
  w.title = utf8(std::wstring(title, tl));
  w.exe = exeOf(pid);
  w.w = r.right - r.left;
  w.h = r.bottom - r.top;
  if (w.w < 64 || w.h < 64) return false;
  HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof mi};
  if (GetMonitorInfoW(m, &mi))
    w.fullscreen = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
                   r.bottom >= mi.rcMonitor.bottom;
  w.foreground = (h == GetForegroundWindow());
  return true;
}

struct EnumCtx { std::vector<WindowInfo>* out; DWORD self; };

BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
  auto* c = reinterpret_cast<EnumCtx*>(lp);
  WindowInfo w;
  if (fillInfo(h, w) && w.pid != c->self && w.exe != "TextInputHost.exe") c->out->push_back(std::move(w));
  return TRUE;
}

struct MonCtx { std::vector<MonitorInfo>* out; };

BOOL CALLBACK monProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
  auto* c = reinterpret_cast<MonCtx*>(lp);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  if (!GetMonitorInfoW(m, &mi)) return TRUE;
  MonitorInfo i;
  i.index = (int)c->out->size();
  i.name = utf8(mi.szDevice);
  i.w = mi.rcMonitor.right - mi.rcMonitor.left;
  i.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  i.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  c->out->push_back(i);
  return TRUE;
}

}  // namespace

std::vector<WindowInfo> listWindows() {
  std::vector<WindowInfo> v;
  EnumCtx c{&v, GetCurrentProcessId()};
  EnumWindows(enumProc, (LPARAM)&c);
  return v;
}

WindowInfo foregroundWindowInfo() {
  WindowInfo w;
  HWND h = GetForegroundWindow();
  if (h) fillInfo(h, w);
  return w;
}

std::vector<MonitorInfo> listMonitors() {
  std::vector<MonitorInfo> v;
  MonCtx c{&v};
  EnumDisplayMonitors(nullptr, nullptr, monProc, (LPARAM)&c);
  return v;
}

std::vector<AudioDevice> listAudioDevices() {
  std::vector<AudioDevice> out;
  HRESULT hr0 = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
      for (int flow = 0; flow < 2; ++flow) {
        EDataFlow f = flow == 0 ? eRender : eCapture;
        std::wstring defId;
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(f, eConsole, &def))) {
          LPWSTR id = nullptr;
          if (SUCCEEDED(def->GetId(&id))) { defId = id; CoTaskMemFree(id); }
        }
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(f, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
          ComPtr<IMMDevice> d;
          if (FAILED(col->Item(i, &d))) continue;
          LPWSTR id = nullptr;
          if (FAILED(d->GetId(&id))) continue;
          AudioDevice a;
          a.id = utf8(id);
          a.capture = (flow == 1);
          a.isDefault = (defId == id);
          CoTaskMemFree(id);
          ComPtr<IPropertyStore> ps;
          if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) a.name = utf8(pv.pwszVal);
            PropVariantClear(&pv);
          }
          out.push_back(std::move(a));
        }
      }
    }
  }
  if (SUCCEEDED(hr0)) CoUninitialize();
  return out;
}

}  // namespace vv
