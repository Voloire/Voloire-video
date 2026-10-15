# Fetches everything the build and the installers need into %USERPROFILE%\deps (or $env:VV_DEPS).
# Idempotent: a component that is already there is skipped.
#   scripts\setup-deps.ps1
$ErrorActionPreference = 'Stop'
$deps = if ($env:VV_DEPS) { $env:VV_DEPS } else { Join-Path $env:USERPROFILE 'deps' }
New-Item -ItemType Directory -Force $deps | Out-Null

# FFmpeg: LGPL shared build with headers and import libraries.
if (-not (Test-Path (Join-Path $deps 'ffmpeg'))) {
  $dl = Join-Path $deps 'ffmpeg-dl'
  New-Item -ItemType Directory -Force $dl | Out-Null
  gh release download latest -R BtbN/FFmpeg-Builds -p 'ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip' --dir $dl --clobber
  Expand-Archive (Join-Path $dl '*.zip') -DestinationPath (Join-Path $deps 'ffmpeg') -Force
}

# Qt 6.5 LTS for MSVC 2019, through aqtinstall.
if (-not (Test-Path (Join-Path $deps 'qt\6.5.3\msvc2019_64'))) {
  python -m pip install --user --quiet aqtinstall
  python -m aqt install-qt windows desktop 6.5.3 win64_msvc2019_64 -O (Join-Path $deps 'qt')
}

# C++/WinRT projection. The headers bundled with the VS2019 SDK predate standard coroutines.
if (-not (Test-Path (Join-Path $deps 'cppwinrt\winrt\base.h'))) {
  $pkg = Join-Path $deps 'cppwinrt-pkg'
  New-Item -ItemType Directory -Force $pkg | Out-Null
  Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/2.0.210930.14' -OutFile (Join-Path $pkg 'pkg.zip')
  Expand-Archive (Join-Path $pkg 'pkg.zip') -DestinationPath (Join-Path $pkg 'x') -Force
  & (Join-Path $pkg 'x\bin\cppwinrt.exe') -input sdk -output (Join-Path $deps 'cppwinrt') | Out-Null
}

# WiX 3.14 (MS-RL, no maintenance fee) for the .msi.
if (-not (Test-Path (Join-Path $deps 'wix3\candle.exe'))) {
  $w = Join-Path $deps 'wix3'
  New-Item -ItemType Directory -Force $w | Out-Null
  Invoke-WebRequest 'https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip' -OutFile (Join-Path $w 'w.zip')
  Expand-Archive (Join-Path $w 'w.zip') -DestinationPath $w -Force
}

# Inno Setup 6 for the .exe.
$inno = Get-ChildItem "$env:LOCALAPPDATA\Programs\Inno Setup 6", 'C:\Program Files (x86)\Inno Setup 6' -Filter ISCC.exe -ErrorAction SilentlyContinue
if (-not $inno) { winget install --id JRSoftware.InnoSetup -e --scope user --silent --accept-package-agreements --accept-source-agreements }

# Python test dependency.
python -m pip install --user --quiet numpy
Write-Host "dependencies ready in $deps"
