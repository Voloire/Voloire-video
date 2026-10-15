#pragma once

namespace vv {

// Writes exception code, faulting address and a symbolised stack to stderr and to
// %LOCALAPPDATA%\Voloire-video\crash.log, then lets the process die.
void installCrashHandler();

}  // namespace vv
