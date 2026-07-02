# Dropkick on Windows (build from source)

Dropkick was built for the Raspberry Pi, but it also builds and runs on Windows
with desktop OpenGL. The remote (browse, favorites, live preset editor, settings),
preset packs, and the GPU-hang auto-skip all work. Two things do **not** apply on
Windows: the **Reduce flashing** filter (GLES-only, inert here) and the Linux-only
watchdog / systemd / launcher.

## Prerequisites (install once)
- **Git** — https://git-scm.com
- **CMake** ≥ 3.27 — https://cmake.org (tick "Add to PATH")
- **Visual Studio 2022 Build Tools** with the **"Desktop development with C++"**
  workload — https://visualstudio.microsoft.com/downloads/

## Build
```powershell
git clone https://github.com/s66lem/dropkick.git
cd dropkick
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```
`bootstrap.ps1` fetches/bootstraps **vcpkg**, builds libprojectM + the frontend,
installs them under `%USERPROFILE%\.local`, and writes a config + starter files.
The first run compiles the vcpkg dependencies (SDL2, Poco, …) and can take a
while.

## Run
```
%USERPROFILE%\.local\bin\projectMSDL.exe
```
Open the remote from any device on your network at `http://<pc-ip>:8080`
(or `http://localhost:8080` on the PC itself).

## Presets, config, editing
- Drop preset packs under `%USERPROFILE%\.local\share\dropkick\presets\<pack>\`.
- Config lives in `projectMSDL.properties` next to the exe
  (`%USERPROFILE%\.local\bin\`); edit it and restart.
- Author presets in the remote's editor, or drop `.milk` files into
  `%USERPROFILE%\.local\share\dropkick\workshop\` for live hot-reload.

## Notes
- Audio uses WASAPI; pick the input from the remote's Settings (Audio input).
- Unicode preset filenames: the remote handles standard names; very unusual
  characters may not round-trip perfectly on Windows.
