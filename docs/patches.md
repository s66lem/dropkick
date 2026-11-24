# Dropkick Patch Set

Both patches make projectM run on GLES 3.1 drivers (e.g. Raspberry Pi 5 V3D)
via runtime capability detection rather than hardcoded downgrades. Build both
forks with `-DENABLE_GLES=TRUE` on the Pi (bootstrap does this).

## 01 — libprojectM GLES acceptance floor
`patches/01-libprojectm-gles-floor.patch`
Lowers the GLES minimum from 3.2 / 3.20 to 3.1 / 3.10 in
`src/libprojectM/Renderer/Platform/GladLoader.cpp`. This is a floor
(accept ≥ 3.1), not a version request.

## 02 — frontend GLES context fallback ladder
`patches/02-frontend-gles-context-fallback.patch`
`src/SDLRenderingWindow.cpp` requests GLES 3.2, then 3.1, then 3.0, using the
first context that creates successfully. On the Pi 5 this lands on 3.1; on
stronger GPUs it keeps 3.2.

## Source of truth
The patches are exported, documentation-only copies of the GLES changes. The
authoritative source is the vendored forks in `external/projectm` and
`external/frontend-sdl-cpp`, which are committed directly in this repository
(no submodules). See [architecture.md](architecture.md) for how the rest of the
frontend (post-processing, remote, watchdogs) builds on them.
