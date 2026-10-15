# Third-party components

| Component | Use | Licence | Notes |
|---|---|---|---|
| FFmpeg 8.1 (BtbN LGPL shared build) | encoding and muxing | LGPL 2.1+ | linked dynamically; the build has no GPL parts (no x264, x265). Source: https://ffmpeg.org |
| Qt 6.5 (Widgets, Core, Gui) | tray and panel UI | LGPL 3 | linked dynamically; the Qt DLLs shipped next to the executable can be replaced. Source: https://www.qt.io |
| nlohmann/json 3.11.3 | JSON | MIT | header only, `third_party/json.hpp` |
| doctest 2.4.11 | unit tests | MIT | header only, `third_party/doctest.h`, tests only |
| Ubuntu and Ubuntu Mono fonts | UI typography | Ubuntu Font Licence 1.0 | `assets/fonts`, licence in `assets/fonts/UFL.txt` |
| C++/WinRT projection 2.0.210930.14 | Windows Graphics Capture | MIT | generated at build time, not redistributed |
| Visual C++ runtime | app-local CRT | Microsoft redistributable terms | copied into the installers |
