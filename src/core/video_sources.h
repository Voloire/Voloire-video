#pragma once
#include <memory>
#include "sources.h"

namespace vv {

// Windows Graphics Capture of a single window: works windowed and borderless fullscreen.
std::unique_ptr<IVideoSource> makeWgcWindowSource(void* hwnd);
// Windows Graphics Capture of a whole monitor.
std::unique_ptr<IVideoSource> makeWgcMonitorSource(int monitorIndex);
// DXGI Desktop Duplication of a monitor: fallback for exclusive fullscreen.
std::unique_ptr<IVideoSource> makeDxgiSource(int monitorIndex);

}  // namespace vv
