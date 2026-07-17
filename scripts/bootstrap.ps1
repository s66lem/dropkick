# Dropkick — Windows build-from-source bootstrap.
#
# Prerequisites (install once):
#   - Git                         https://git-scm.com
#   - CMake (>= 3.27)             https://cmake.org  (add to PATH)
#   - Visual Studio 2022 Build Tools with the "Desktop development with C++" workload
#
# Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
#
# Builds the bundled libprojectM + frontend with desktop OpenGL (WASAPI audio),
# using vcpkg for dependencies. The strobe "Reduce flashing" filter is GLES-only,
# so it is inert on Windows; everything else (remote, workshop, favorites,
# settings, auto-skip) works.
$ErrorActionPreference = "Stop"
$Root   = Split-Path -Parent $PSScriptRoot
$Prefix = Join-Path $env:USERPROFILE ".local"
$Data   = Join-Path $env:USERPROFILE ".local\share\dropkick"
$DataFwd = $Data -replace '\\','/'

Write-Host "== vcpkg =="
$Vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $Root "vcpkg" }
if (-not (Test-Path (Join-Path $Vcpkg "vcpkg.exe"))) {
    if (-not (Test-Path $Vcpkg)) { git clone https://github.com/microsoft/vcpkg.git $Vcpkg }
    & (Join-Path $Vcpkg "bootstrap-vcpkg.bat")
}
$Toolchain = Join-Path $Vcpkg "scripts\buildsystems\vcpkg.cmake"

Write-Host "== Build libprojectM =="
cmake -S "$Root\external\projectm" -B "$Root\external\projectm\build" `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$Prefix"
cmake --build "$Root\external\projectm\build" --config Release
cmake --install "$Root\external\projectm\build" --config Release

Write-Host "== Build frontend =="
cmake -S "$Root\external\frontend-sdl-cpp" -B "$Root\external\frontend-sdl-cpp\build" `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$Prefix" -DCMAKE_INSTALL_PREFIX="$Prefix"
cmake --build "$Root\external\frontend-sdl-cpp\build" --config Release
cmake --install "$Root\external\frontend-sdl-cpp\build" --config Release

# The frontend's install() drops projectMSDL.exe at the prefix root and does not
# bundle its vcpkg runtime DLLs, while libprojectM installs its DLLs under bin\.
# Stage one self-contained bin\ so the exe finds every DLL (and the configured
# projectMSDL.properties written below) next to it.
$FrontRel = "$Root\external\frontend-sdl-cpp\build\src\Release"
$BinDir   = Join-Path $Prefix "bin"
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
Copy-Item "$FrontRel\projectMSDL.exe" $BinDir -Force
Copy-Item "$FrontRel\*.dll" $BinDir -Force
# Drop the stray prefix-root copies (exe beside a stub .properties, no DLLs).
Remove-Item (Join-Path $Prefix "projectMSDL.exe"), (Join-Path $Prefix "projectMSDL.properties") -Force -ErrorAction SilentlyContinue

Write-Host "== Data dirs, assets, config =="
foreach ($d in "presets\cream-of-the-crop","textures","remote","workshop","state") {
    New-Item -ItemType Directory -Force -Path (Join-Path $Data $d) | Out-Null
}
Copy-Item "$Root\remote\*" (Join-Path $Data "remote") -Force
if (-not (Test-Path (Join-Path $Data "workshop\starter.milk"))) {
    Copy-Item "$Root\share\starter.milk" (Join-Path $Data "workshop\starter.milk") -Force
}

# The frontend loads <exe-dir>\projectMSDL.properties. Write it with Windows paths.
$PropsPath = Join-Path $Prefix "bin\projectMSDL.properties"

# Reuse an existing remote token if the properties file already has one, otherwise
# generate a random one so the phone remote isn't wide open to the whole LAN by
# default. Blank remote.token in the file to run an open remote.
$Token = ""
if (Test-Path $PropsPath) {
    $existing = Select-String -Path $PropsPath -Pattern '^\s*remote\.token\s*=\s*(\S+)' | Select-Object -First 1
    if ($existing) { $Token = $existing.Matches[0].Groups[1].Value }
}
if (-not $Token) {
    $Token = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
    Write-Host "Generated a remote-control token: $Token"
}

# Windowed by default on Windows: a borderless-fullscreen window opened at launch
# lands behind whatever has foreground and looks like the app died. Press F to
# toggle fullscreen.
$props = @"
window.fullscreen = false
window.width = 1280
window.height = 720
projectM.presetPath = $DataFwd/presets/cream-of-the-crop
projectM.texturePath = $DataFwd/textures
projectM.shuffleEnabled = true
projectM.presetLocked = false
audio.device =
remote.port = 8080
remote.token = $Token
remote.presetRoot = $DataFwd/presets
remote.webRoot = $DataFwd/remote
remote.workshopDir = $DataFwd/workshop
"@
$props | Out-File -FilePath $PropsPath -Encoding ascii

# A "dropkick" command, like the Linux launcher. %USERPROFILE%\.local\bin must be
# on PATH for this to work from a shell.
@"
@echo off
start "" /D "%~dp0" "%~dp0projectMSDL.exe" %*
"@ | Out-File -FilePath (Join-Path $BinDir "dropkick.cmd") -Encoding ascii

$RemoteQuery = if ($Token) { "/?token=$Token" } else { "" }
Write-Host ""
Write-Host "Done. Run:  $Prefix\bin\projectMSDL.exe"
Write-Host "Remote:     http://localhost:8080$RemoteQuery  (or http://<this-pc-ip>:8080$RemoteQuery from a phone)"
Write-Host "Add presets under $Data\presets\cream-of-the-crop\"
