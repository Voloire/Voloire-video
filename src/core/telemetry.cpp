#include "telemetry.h"
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace vv {

namespace {
const char* levelName(Level l) {
  switch (l) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    default: return "error";
  }
}
}  // namespace

Telemetry& Telemetry::get() {
  static Telemetry t;
  return t;
}

void Telemetry::startSession(const std::string& dir) {
  endSession();
  std::lock_guard lk(mu_);
  std::filesystem::create_directories(pathOf(dir));
  dir_ = dir;
  events_.open(pathOf(dir) / "events.jsonl", std::ios::trunc);
  metricsOut_.open(pathOf(dir) / "metrics.jsonl", std::ios::trunc);
  t0_ = nowUs();
  running_ = true;
  sampler_ = std::thread([this] { samplerLoop(); });
}

void Telemetry::endSession() {
  if (running_.exchange(false) && sampler_.joinable()) sampler_.join();
  std::lock_guard lk(mu_);
  if (events_.is_open()) events_.close();
  if (metricsOut_.is_open()) metricsOut_.close();
}

void Telemetry::samplerLoop() {
  while (running_) {
    for (int i = 0; i < 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!running_) break;
    json s = snapshot();
    s["t_ms"] = (nowUs() - t0_) / 1000;
    std::lock_guard lk(mu_);
    if (metricsOut_.is_open()) metricsOut_ << s.dump() << '\n' << std::flush;
  }
}

void Telemetry::log(Level lvl, const std::string& component, const std::string& msg, const json& extra) {
  json e = {{"t_us", nowUs()}, {"level", levelName(lvl)}, {"component", component}, {"msg", msg}};
  if (!extra.empty()) e["data"] = extra;
  std::lock_guard lk(mu_);
  if (events_.is_open()) events_ << e.dump() << '\n' << std::flush;
  ring_.push_back(e);
  if (ring_.size() > 2000) ring_.pop_front();
  if (echo_) std::fprintf(stderr, "[%s] %s: %s\n", levelName(lvl), component.c_str(), msg.c_str());
}

void Telemetry::inc(const std::string& name, double by) {
  std::lock_guard lk(mu_);
  metrics_[name] += by;
}

void Telemetry::set(const std::string& name, double value) {
  std::lock_guard lk(mu_);
  metrics_[name] = value;
}

double Telemetry::value(const std::string& name) const {
  std::lock_guard lk(mu_);
  auto it = metrics_.find(name);
  return it == metrics_.end() ? 0.0 : it->second;
}

json Telemetry::snapshot() const {
  std::lock_guard lk(mu_);
  json j = json::object();
  for (auto& [k, v] : metrics_) j[k] = v;
  return j;
}

void Telemetry::resetMetrics() {
  std::lock_guard lk(mu_);
  metrics_.clear();
}

json Telemetry::tailLogs(size_t n) const {
  std::lock_guard lk(mu_);
  json a = json::array();
  size_t start = ring_.size() > n ? ring_.size() - n : 0;
  for (size_t i = start; i < ring_.size(); ++i) a.push_back(ring_[i]);
  return a;
}

std::string Telemetry::sessionDir() const {
  std::lock_guard lk(mu_);
  return dir_;
}

}  // namespace vv
