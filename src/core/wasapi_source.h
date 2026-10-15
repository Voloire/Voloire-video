#pragma once
#include <memory>
#include <string>
#include "sources.h"

namespace vv {

// Audio of one process (and its children) via WASAPI process loopback. Windows 10 2004+ / 11.
std::unique_ptr<IAudioSource> makeProcessLoopbackSource(uint32_t pid);

// A capture endpoint: a microphone, or with loopback=true the mix of a render endpoint.
// deviceId empty selects the default device of that kind.
std::unique_ptr<IAudioSource> makeEndpointSource(const std::string& deviceId, bool loopback);

}  // namespace vv
