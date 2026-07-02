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

Write-Host "== Data dirs, assets, config =="
foreach ($d in "presets\cream-of-the-crop","textures","remote","workshop","state") {
    New-Item -ItemType Directory -Force -Path (Join-Path $Data $d) | Out-Null
}
Copy-Item "$Root\remote\*" (Join-Path $Data "remote") -Force
if (-not (Test-Path (Join-Path $Data "workshop\starter.milk"))) {
    Copy-Item "$Root\share\starter.milk" (Join-Path $Data "workshop\starter.milk") -Force
}

# The frontend loads <exe-dir>\projectMSDL.properties. Write it with Windows paths.
$props = @"
window.fullscreen = true
projectM.presetPath = $DataFwd/presets/cream-of-the-crop
projectM.texturePath = $DataFwd/textures
projectM.shuffleEnabled = true
projectM.presetLocked = false
audio.device =
remote.port = 8080
remote.token =
remote.presetRoot = $DataFwd/presets
remote.webRoot = $DataFwd/remote
remote.workshopDir = $DataFwd/workshop
"@
$props | Out-File -FilePath (Join-Path $Prefix "bin\projectMSDL.properties") -Encoding ascii

Write-Host ""
Write-Host "Done. Run:  $Prefix\bin\projectMSDL.exe"
Write-Host "Remote:     http://localhost:8080  (or http://<this-pc-ip>:8080 from a phone)"
Write-Host "Add presets under $Data\presets\cream-of-the-crop\"
