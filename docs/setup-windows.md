# Dropkick on Windows (build from source)

Dropkick was built for the Raspberry Pi, but it also builds and runs on Windows
with desktop OpenGL 3.3. Everything in the remote works (browse, favorites, live
preset editor, settings), as do preset packs, the GPU-hang auto-skip, and the
visual controls — **Reduce flashing**, **Brightness**, and **Screen tint** use the
same post-process pipeline as the Pi. Only the Linux-specific watchdog, systemd
unit, and launcher script don't apply.

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
installs them under `%USERPROFILE%\.local`, writes a config and starter files,
adds a private-network Windows Firewall rule for the exe (needs an admin shell;
otherwise it prints the one-line command), and creates a `dropkick` command. The
first run compiles the vcpkg dependencies (SDL2, Poco, …) and can take a while.

## Run
```
dropkick
```
(or `%USERPROFILE%\.local\bin\projectMSDL.exe`; add `%USERPROFILE%\.local\bin` to
`PATH` for the `dropkick` command). It starts windowed; `Ctrl+F` or right-click
toggles fullscreen.

Open the remote from any device on your network at `http://<pc-ip>:8080/op1`
(or `http://localhost:8080/op1` on the PC itself). The bootstrap output prints
your LAN address. `op1` is the default access token; see below to change it.

## Presets, config, editing
- Drop preset packs under `%USERPROFILE%\.local\share\dropkick\presets\<pack>\`.
- Config lives in `projectMSDL.properties` next to the exe
  (`%USERPROFILE%\.local\bin\`); edit it and restart. Set `remote.token` to
  rotate the token (the URL path follows it) or blank it for an open remote.
- Author presets in the remote's editor, or drop `.milk` files into
  `%USERPROFILE%\.local\share\dropkick\workshop\` for live hot-reload.
  See [authoring.md](authoring.md).

## Notes
- Audio uses WASAPI; pick the input from the remote's Settings (Audio input).
- If a phone can't reach the remote, check the firewall rule and that the PC's
  network profile is **Private**.
- Unicode preset filenames: the remote handles standard names; very unusual
  characters may not round-trip perfectly on Windows.
