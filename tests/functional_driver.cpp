// Test-only JSON-line bridge. The allowlist cannot start capture, encoding or a browser.
#include <algorithm>
#include <iostream>
#include "app_controller.h"
#include "links.h"

int main() {
  vv::AppController app;
  vv::LinkTracker links;
  const std::vector<std::string> allowed = {
      "ping", "status", "metrics", "logs", "get_config", "set_config", "set_stream_key", "links",
      "twitch_status", "twitch_auth_start", "twitch_auth_cancel", "twitch_sign_out", "twitch_ingests",
      "twitch_search_categories", "twitch_fetch_key", "twitch_get_info", "twitch_set_info"};
  for (std::string line; std::getline(std::cin, line);) {
    vv::json out;
    try {
      auto req = vv::json::parse(line);
      auto cmd = req.value("cmd", std::string());
      if (cmd == "test_links_begin") {
        links.begin(vv::StreamChannel::fromJson(req.at("channel")), vv::loadUiPrefs().twitchClientId);
        out = links.status().toJson();
      } else if (cmd == "test_links_end") {
        links.streamEnded();
        out = links.status().toJson();
      } else if (cmd == "test_links_status") {
        out = links.status().toJson();
      } else if (cmd == "test_prefs") {
        out = vv::loadUiPrefs().toJson();
      } else if (std::find(allowed.begin(), allowed.end(), cmd) != allowed.end()) {
        out = app.handle(req);
      } else {
        out = {{"ok", false}, {"error", "command disabled in functional driver"}};
      }
    } catch (const std::exception&) {
      out = {{"ok", false}, {"error", "invalid functional request"}};
    }
    std::cout << out.dump() << std::endl;
  }
}
