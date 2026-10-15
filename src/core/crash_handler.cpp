#include "crash_handler.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include "common.h"

namespace vv {

namespace {

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409)
    return EXCEPTION_CONTINUE_SEARCH;  // first-chance filter: C++ exceptions and debugger noise pass through
  std::string out;
  char line[512];
  std::snprintf(line, sizeof line, "unhandled exception 0x%08lX at %p\n", ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
  out += line;

  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, nullptr, TRUE);
  // The faulting thread's own stack: RIP may sit in unloaded code, so besides the unwound
  // frames, scan the raw stack words for return addresses that fall in loaded modules.
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto describe = [&](DWORD64 addr, const char* tag) {
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_MODULE64 mod{sizeof mod};
    const char* mname = SymGetModuleInfo64(proc, addr, &mod) ? mod.ModuleName : "?";
    if (SymFromAddr(proc, addr, &disp, sym)) {
      std::snprintf(line, sizeof line, "  %s %s!%s +0x%llx\n", tag, mname, sym->Name, disp);
    } else {
      std::snprintf(line, sizeof line, "  %s %s %p\n", tag, mname, (void*)addr);
    }
    out += line;
  };
  CONTEXT ctx = *ep->ContextRecord;
  describe(ctx.Rip, "rip");
  DWORD64* sp = reinterpret_cast<DWORD64*>(ctx.Rsp);
  int shown = 0;
  for (int i = 0; i < 400 && shown < 24; ++i) {
    DWORD64 v = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, sp + i, &v, sizeof v, &got) || got != sizeof v) break;
    IMAGEHLP_MODULE64 mod{sizeof mod};
    if (v > 0x10000 && SymGetModuleInfo64(proc, v, &mod)) {
      describe(v, "stk");
      ++shown;
    }
  }
  std::fputs(out.c_str(), stderr);
  try {
    wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (base) {
      auto dir = std::filesystem::path(base) / L"Voloire-video";
      std::filesystem::create_directories(dir);
      FILE* f = nullptr;
      _wfopen_s(&f, (dir / L"crash.log").c_str(), L"ab");
      if (f) { std::fputs(out.c_str(), f); std::fclose(f); }
    }
  } catch (...) {}
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void installCrashHandler() {
  AddVectoredExceptionHandler(1, onCrash);
  SetUnhandledExceptionFilter(onCrash);
}

}  // namespace vv
