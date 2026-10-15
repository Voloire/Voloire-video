#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include "common.h"

namespace vv {

// Line-delimited JSON over a local named pipe: one request object per line, one reply per line.
// The same channel serves the UI, the CLI and the end-to-end tests.
class ControlServer {
 public:
  using Handler = std::function<json(const json&)>;
  ~ControlServer() { stop(); }
  bool start(const std::string& pipeName, Handler h, std::string& err);
  void stop();

 private:
  void loop();
  std::string path_;
  Handler handler_;
  std::atomic<bool> run_{false};
  void* stopEvent_ = nullptr;
  void* firstPipe_ = nullptr;  // instance created in start() so clients never race the thread
  std::thread th_;
};

}  // namespace vv
