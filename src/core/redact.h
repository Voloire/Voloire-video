#pragma once
#include <string>
#include "common.h"

namespace vv {

// Stream keys must never reach a log, a status reply or a file. These helpers strip them at the
// one choke point every event passes through (Telemetry::log), and anywhere else they could slip.

// A copy of j with the values of "key" and "streamKey" (case-insensitive) replaced by "***".
json redactJson(const json& j);
// rtmp[s]://host[:port]/app/<anything> becomes rtmp[s]://host[:port]/app/***. Other strings pass
// through untouched.
std::string redactUrl(const std::string& url);
// Applies redactUrl to every string inside j, in place.
void redactUrls(json& j);

}  // namespace vv
