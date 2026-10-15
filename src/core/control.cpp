#include "control.h"
#include <windows.h>
#include "telemetry.h"

namespace vv {

namespace {

// Overlapped read of one byte-stream chunk, abortable through stopEv. Returns bytes read or -1.
long readSome(HANDLE pipe, HANDLE stopEv, char* buf, DWORD cap) {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD n = 0;
  long result = -1;
  if (!ReadFile(pipe, buf, cap, &n, &ov) && GetLastError() == ERROR_IO_PENDING) {
    HANDLE h[2] = {ov.hEvent, stopEv};
    DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &ov, &n, FALSE)) result = (long)n;
    else CancelIoEx(pipe, &ov);
  } else if (GetLastError() != ERROR_BROKEN_PIPE) {
    result = (long)n;
  }
  if (result == -1 && n == 0) { /* disconnect or abort */ }
  CloseHandle(ov.hEvent);
  return result;
}

bool writeAll(HANDLE pipe, const std::string& s) {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD n = 0;
  bool ok = WriteFile(pipe, s.data(), (DWORD)s.size(), &n, &ov) || GetLastError() == ERROR_IO_PENDING;
  if (ok) ok = GetOverlappedResult(pipe, &ov, &n, TRUE) != 0;
  CloseHandle(ov.hEvent);
  return ok;
}

}  // namespace

bool ControlServer::start(const std::string& pipeName, Handler h, std::string& err) {
  path_ = "\\\\.\\pipe\\" + pipeName;
  handler_ = std::move(h);
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // Probe that the name is free before spawning the thread.
  HANDLE probe = CreateNamedPipeW(wide(path_).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4, 65536, 65536, 0, nullptr);
  if (probe == INVALID_HANDLE_VALUE) {
    err = "control pipe " + path_ + " is already in use";
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    return false;
  }
  firstPipe_ = probe;
  run_ = true;
  th_ = std::thread([this] { loop(); });
  VV_LOG(Info, "control", "control pipe listening", {{"pipe", path_}});
  return true;
}

void ControlServer::stop() {
  if (!run_.exchange(false)) return;
  SetEvent(stopEvent_);
  if (th_.joinable()) th_.join();
  CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
}

void ControlServer::loop() {
  HANDLE stopEv = stopEvent_;
  while (run_) {
    HANDLE pipe = firstPipe_ ? (HANDLE)firstPipe_
                             : CreateNamedPipeW(wide(path_).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4,
                                   65536, 65536, 0, nullptr);
    firstPipe_ = nullptr;
    if (pipe == INVALID_HANDLE_VALUE) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (ConnectNamedPipe(pipe, &ov)) connected = true;
    else if (GetLastError() == ERROR_PIPE_CONNECTED) connected = true;
    else if (GetLastError() == ERROR_IO_PENDING) {
      HANDLE h[2] = {ov.hEvent, stopEv};
      if (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0) connected = true;
      else CancelIoEx(pipe, &ov);
    }
    CloseHandle(ov.hEvent);
    if (connected) {
      std::string acc;
      char buf[4096];
      while (run_) {
        size_t nl;
        while ((nl = acc.find('\n')) == std::string::npos) {
          long n = readSome(pipe, stopEv, buf, sizeof buf);
          if (n <= 0) goto done;
          acc.append(buf, (size_t)n);
        }
        std::string line = acc.substr(0, nl);
        acc.erase(0, nl + 1);
        if (line.empty() || line == "\r") continue;
        json reply;
        try {
          reply = handler_(json::parse(line));
        } catch (const std::exception& e) {
          reply = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
        }
        if (!writeAll(pipe, reply.dump() + "\n")) goto done;
      }
    }
  done:
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

}  // namespace vv
