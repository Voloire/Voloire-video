# Builds Release, stages the app and produces both installers in dist\:
#   voloire-video-setup-<version>.exe   (Inno Setup, per-user)
#   voloire-video-<version>.msi         (WiX 3.14, per-user)
#   scripts\package.ps1 [-SkipBuild] [-Version 1.0.0]
param(
  [switch]$SkipBuild,
  [string]$Version
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = if ($env:VV_DEPS) { $env:VV_DEPS } else { Join-Path $env:USERPROFILE 'deps' }
if (-not $Version) {
  $Version = ((Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(voloire_video VERSION ([0-9.]+)').Matches[0].Groups[1].Value)
}
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') }

$bin = Join-Path $root 'build\bin'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'stage'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $stage | Out-Null

# Application, Qt and FFmpeg runtime; leave out the test tools, PDBs and the redistributable installer.
$skip = 'vv_unit_tests.exe', 'vv_functional_driver.exe', 'vv_ui_functional_tests.exe', 'testsubject.exe', 'vc_redist.x64.exe'
Get-ChildItem $bin -File | Where-Object { $skip -notcontains $_.Name -and $_.Extension -ne '.pdb' } | Copy-Item -Destination $stage
Get-ChildItem $bin -Directory | ForEach-Object { Copy-Item $_.FullName (Join-Path $stage $_.Name) -Recurse }

# App-local Visual C++ runtime, so the installer needs no separate redistributable.
$pf86 = if (${env:ProgramFiles(x86)}) { ${env:ProgramFiles(x86)} } else { 'C:\Program Files (x86)' }
$vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending |
  ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } | Select-Object -First 1
if (-not $crt) { throw 'Visual C++ runtime redistributable folder not found' }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage

Copy-Item (Join-Path $root 'LICENSE') $stage
Copy-Item (Join-Path $root 'THIRD-PARTY.md') $stage
Copy-Item (Join-Path $root 'assets\fonts\UFL.txt') (Join-Path $stage 'fonts')

# ---- .exe (Inno Setup)
$iscc = (Get-ChildItem "$env:LOCALAPPDATA\Programs\Inno Setup 6", 'C:\Program Files (x86)\Inno Setup 6', 'C:\Program Files\Inno Setup 6' -Filter ISCC.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }
& $iscc /Qp "/DAppVersion=$Version" "/DStageDir=$stage" "/DRepoDir=$root" (Join-Path $root 'installer\voloire-video.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }

# ---- .msi (WiX 3.14, MS-RL licence: no fee tier)
$wix = if ($env:VV_WIX3) { $env:VV_WIX3 } else { Join-Path $deps 'wix3' }
if (-not (Test-Path (Join-Path $wix 'candle.exe'))) { throw "WiX 3.14 binaries not found in $wix (scripts\setup-deps.ps1 fetches them)" }
# license.rtf for the MSI wizard, generated from LICENSE
$lic = (Get-Content (Join-Path $root 'LICENSE') -Raw) -replace '\\', '\\' -replace '\{', '\{' -replace '\}', '\}' -replace "`r?`n", '\par '
Set-Content -Encoding ascii (Join-Path $root 'installer\license.rtf') ('{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\f0\fs20 ' + $lic + '}')
$obj = Join-Path $dist 'wixobj'
New-Item -ItemType Directory -Force $obj | Out-Null
& (Join-Path $wix 'heat.exe') dir $stage -cg AppFiles -dr INSTALLFOLDER -gg -sfrag -srd -sreg -var var.StageDir -out (Join-Path $obj 'files.wxs') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'heat failed' }
& (Join-Path $wix 'candle.exe') -nologo -arch x64 "-dStageDir=$stage" "-dVersion=$Version" "-dRepoDir=$root" -out "$obj\" (Join-Path $root 'installer\voloire-video.wxs') (Join-Path $obj 'files.wxs')
if ($LASTEXITCODE -ne 0) { throw 'candle failed' }
$msi = Join-Path $dist "voloire-video-$Version.msi"
& (Join-Path $wix 'light.exe') -nologo -ext (Join-Path $wix 'WixUIExtension.dll') -sice:ICE38 -sice:ICE64 -sice:ICE91 -sice:ICE03 -sice:ICE60 -out $msi (Join-Path $obj 'voloire-video.wixobj') (Join-Path $obj 'files.wixobj')
if ($LASTEXITCODE -ne 0) { throw 'light failed' }

Remove-Item (Join-Path $dist '*.wixpdb') -ErrorAction SilentlyContinue
Get-ChildItem $dist -File | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table | Out-String | Write-Host
