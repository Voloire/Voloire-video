#pragma once
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>
#include "common.h"

namespace vv {

enum class Level { Debug, Info, Warn, Error };

// Structured event log (JSON lines) plus a metric registry sampled once per second.
// One process-wide instance; every component reports through it, nothing prints on its own.
class Telemetry {
 public:
  static Telemetry& get();

  // Opens <dir>/events.jsonl and <dir>/metrics.jsonl and starts the 1 Hz sampler.
  void startSession(const std::string& dir);
  void endSession();

  void log(Level lvl, const std::string& component, const std::string& msg, const json& extra = json::object());

  // Metrics: counters accumulate, gauges hold the last value.
  void inc(const std::string& name, double by = 1);
  void set(const std::string& name, double value);
  double value(const std::string& name) const;
  json snapshot() const;
  void resetMetrics();

  json tailLogs(size_t n) const;
  void setConsoleEcho(bool on) { echo_ = on; }
  std::string sessionDir() const;

 private:
  Telemetry() = default;
  void samplerLoop();

  mutable std::mutex mu_;
  std::map<std::string, double> metrics_;
  std::deque<json> ring_;
  std::ofstream events_, metricsOut_;
  std::string dir_;
  std::atomic<bool> echo_{false};
  std::atomic<bool> running_{false};
  std::thread sampler_;
  int64_t t0_ = 0;
};

#define VV_LOG(lvl, comp, msg, ...) ::vv::Telemetry::get().log(::vv::Level::lvl, comp, msg, ##__VA_ARGS__)

}  // namespace vv
