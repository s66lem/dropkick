# Making your own visualizations

Dropkick visualizations are **Milkdrop `.milk` presets** — text files of equations
that drive motion, color, and waveforms, reacting to the audio in real time.

## The workshop loop

There are two ways to iterate; both land in
`~/.local/share/dropkick/workshop/` (`%USERPROFILE%\.local\share\dropkick\workshop\`
on Windows).

**In the remote's editor**
1. Open the editor with **Edit in Workshop** on **Now Playing** (current preset) or
   **Edit current** on **Browse**.
2. Edit the preset text. Press **Ctrl+Enter** (or **Apply**) to write it to
   `workshop/_scratch.milk` and send that to the display. It is a scratch file,
   overwritten on every Apply.
3. When you like it, press **Save** and name it in the inline dialog. It is
   written to `workshop/` as a normal workshop preset. If a file with that name
   already exists, you are asked to confirm before it is overwritten.

**In your own editor**
1. Edit any `.milk` in `workshop/` (VS Code, nano, …). When you **save**, Dropkick
   live-reloads it on the TV within about a second.
2. To start from something on screen, open it with **Edit in Workshop** (Now Playing)
   or **Edit current** (Browse) and **Save** it into `workshop/` (or call
   `POST /api/workshop/capture`), then edit that file on disk.

When a workshop preset is live, the remote shows **Workshop · Live**. Hit
Next/Random or pick from Browse to leave it and return to the library.

> Reaching the files from your laptop: share `~/.local/share/dropkick/workshop/`
> over your network (Samba/SFTP), or `git`/`scp` files onto the Pi. Then point
> your editor at it and save to iterate.

## Editing `starter.milk`

`starter.milk` is a minimal, working preset. The lines to play with:

    per_frame_1=rot = rot + 0.02*sin(time);   // rotate, wobbling over time
    per_frame_2=zoom = 1.0 + 0.02*bass;        // pulse zoom with the bass
    per_frame_3=wave_r = 0.5 + 0.5*sin(time*1.13);  // cycle the waveform's red
    per_frame_4=wave_g = 0.5 + 0.5*sin(time*1.23 + 2.0);
    per_frame_5=wave_b = 0.5 + 0.5*sin(time*1.33 + 4.0);

### Recipes — change one line, watch the TV
- **Faster spin:** raise the `0.02` in `per_frame_1` to `0.10`.
- **Harder bass pulse:** change `per_frame_2` to `zoom = 1.0 + 0.08*bass;`
- **React to treble instead of bass:** use `treb` in place of `bass`.
- **Add drift:** add a line `per_frame_6=dx = 0.01*sin(time*0.7);` (pans the image).
- **Trails:** lower `fDecay` (e.g. `fDecay=0.98` → `0.94` leaves longer trails; closer to `1.0` = cleaner).

## The variables you'll use most
Audio (per frame): `bass`, `mid`, `treb`, and their smoothed forms
`bass_att`, `mid_att`, `treb_att`. `time` is seconds since start; `frame` is the
frame counter.

Motion (assign in `per_frame_N`):
- `zoom` — 1.0 = none, >1 zooms in. `rot` — rotation. `cx`/`cy` — center of zoom/rotation (0..1).
- `dx`/`dy` — pan. `sx`/`sy` — stretch. `warp` — warping amount. `fDecay` — how fast the previous frame fades.

Waveform: `wave_r/g/b/a` color, `wave_x/wave_y` position, `fWaveScale` size,
`nWaveMode` (0–7) shape.

Custom variables: `q1`–`q32` carry values from `per_frame` into `per_pixel`/shaders;
`t1`–`t8` are per-preset scratch. Everything is the Milkdrop expression language
(math: `sin cos abs pow min max if(cond,a,b)` …).

## Shaders and the Pi
Presets can carry `warp`/`composite` HLSL shaders. On the Pi these cross-compile
to **GLSL ES 3.10**, so very advanced shaders may render differently or fail
(desktop OpenGL builds are more forgiving).
Equation-driven presets (like `starter.milk`, `fShader=0`) are the portable,
reliable path. If a fancy preset looks wrong on the Pi, that's usually why.

## Keeping a preset
When you're happy, copy the file out of `workshop/` into a pack under
`~/.local/share/dropkick/presets/<pack>/` and it becomes part of the rotation.

## Learn more
- Milkdrop Preset Authoring Guide (search "MilkDrop preset authoring geiss").
- Butterchurn (butterchurn.org) — a browser Milkdrop with a live editor, handy
  for experimenting before dropping a `.milk` into `workshop/`.
