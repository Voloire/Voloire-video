# Configure and build Voloire-video with the MSVC toolchain.
#   scripts\build.ps1 [-Config Release] [-NoUi] [-Clean]
# Dependencies are looked up through environment variables, with the layout used on capybara
# as default: VV_DEPS (FFmpeg shared dev build in ffmpeg\*, Qt in qt\6.5.3\msvc2019_64).
param(
  [string]$Config = 'Release',
  [switch]$NoUi,
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = if ($env:VV_DEPS) { $env:VV_DEPS } else { Join-Path $env:USERPROFILE 'deps' }

$ffmpeg = if ($env:VV_FFMPEG_ROOT) { $env:VV_FFMPEG_ROOT } else { (Get-ChildItem (Join-Path $deps 'ffmpeg') -Directory | Select-Object -First 1).FullName }
$qt = if ($env:VV_QT_ROOT) { $env:VV_QT_ROOT } else { Join-Path $deps 'qt\6.5.3\msvc2019_64' }

$pf86 = if (${env:ProgramFiles(x86)}) { ${env:ProgramFiles(x86)} } else { 'C:\Program Files (x86)' }
[Environment]::SetEnvironmentVariable('ProgramFiles(x86)', $pf86, 'Process')  # vcvars needs it; MSYS shells drop it
$vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC build tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = if (Get-Command cmake -ErrorAction SilentlyContinue) { 'cmake' } else { Join-Path $cmakeDir 'CMake\bin\cmake.exe' }
$ninja = Join-Path $cmakeDir 'Ninja'

$cppwinrt = if ($env:VV_CPPWINRT_INCLUDE) { $env:VV_CPPWINRT_INCLUDE } else { Join-Path $deps 'cppwinrt' }
$build = Join-Path $root 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$ui = if ($NoUi) { 'OFF' } else { 'ON' }
$cfgArgs = "-S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DVV_FFMPEG_ROOT=`"$ffmpeg`" -DVV_BUILD_UI=$ui -DVV_CPPWINRT_INCLUDE=`"$cppwinrt`" -DCMAKE_PREFIX_PATH=`"$qt`""
$cmd = "set `"PATH=$pf86\Microsoft Visual Studio\Installer;$ninja;%PATH%`" && `"$vcvars`" >nul && `"$cmake`" $cfgArgs && `"$cmake`" --build `"$build`""
$ErrorActionPreference = 'Continue'   # vcvars prints harmless notes on stderr
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built: $build\bin"
