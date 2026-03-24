# Structor — Claude Code context

## What this is
Musique concrete & acousmatique-inspired real-time sound deconstructor/reconstructor for Ableton Move.

Plugin type: audio_fx
Module ID: `structor`
API: `audio_fx_api_v2_t`
Entry: `move_audio_fx_init_v2(const host_api_v1_t *host)`
Language: C

Read `design-spec.md` for full design intent. This file is the compressed version.

---

## Sonic intent
Inspired by micro-montage techniques from musique concrete (Schaeffer, Parmegiani).
Deconstructs audio into detected events (onsets, peaks, zero-crossings), then
reconstructs them in 8 algorithmic modes — from random chaos to pitch-sorted sweeps
to density arpeggiators to the tape-cut "Deltarupt" to the spectral-density hybrid
"Spec Warp." Not a granular synth, not a delay, not a spectral processor.

---

## DSP architecture
Audio enters a circular ring buffer (~3 sec, 131072 samples stereo @ 44.1 kHz).
An envelope follower + FFT-based spectral estimator (pffft, 256-point real FFT with
parabolic interpolation) identifies events (onsets, peaks, spectral center frequency).
Events are culled by amplitude (Density param). A mode-dependent algorithm reorders
events (shuffle, sort, arp cycle, spectral-density warp). Grain-based synthesis reads
back events with Hann windowing and Hermite interpolation. Deltarupt mode uses a
quadratic attack envelope with instant cutoff. Output passes through tanh soft limiting
and wet/dry mixing. Feedback recirculates output back into the buffer.

FFT state (PFFFT_Setup + aligned buffers) is pre-allocated in create() — zero
allocation in the render path.

---

## Parameters

### Knobs 1–6: Global

| Knob | Key | Label | Type | Range | Default | Step |
|------|-----|-------|------|-------|---------|------|
| 1 | `detection` | Detection | Float | 0.0–1.0 | 0.1 | 0.01 |
| 2 | `density` | Density | Float | 0.1–1.0 | 0.7 | 0.01 |
| 3 | `grain_size` | Grain Size | Float | 0.5–2.0 | 1.0 | 0.01 |
| 4 | `time_warp` | Time Warp | Float | 0.5–2.0 | 1.0 | 0.01 |
| 5 | `mix` | Mix | Float | 0.0–1.0 | 0.5 | 0.01 |
| 6 | `feedback` | Feedback | Float | 0.0–0.95 | 0.2 | 0.01 |

### Knob 7: Mode

| Knob | Key | Label | Type | Range | Default | Step |
|------|-----|-------|------|-------|---------|------|
| 7 | `mode` | Mode | Enum | 0–7 | 0 | 1 |

Modes: 0=Random, 1=Pitch ↑, 2=Pitch ↓, 3=Density ↑, 4=Time Warp, 5=Dens Arp, 6=Deltarupt, 7=Spec Warp

### Knob 8: Mode-specific (context-sensitive)

| Mode | Key | Label | Type | Range | Default |
|------|-----|-------|------|-------|---------|
| 0 — Random | `shuffle_bias` | Shfl Bias | Float | 0.0–1.0 | 0.5 |
| 1 — Pitch ↑ | `pitch_range_window` | Pitch Win | Float | 0.0–1.0 | 0.0 |
| 2 — Pitch ↓ | `octave_fold` | Oct Fold | Enum | 0–4 | 0 |
| 3 — Density ↑ | `density_curve` | Dens Crv | Float | 0.0–1.0 | 0.0 |
| 4 — Time Warp | `speed_curve_exp` | Spd Curve | Float | 0.5–2.0 | 1.0 |
| 5 — Dens Arp | `arp_pattern` | Arp Ptrn | Enum | 0–5 | 0 |
| 6 — Deltarupt | `deltarupt_attack` | Attack | Float | 0.0–1.0 | 0.05 |
| 7 — Spec Warp | `density_morphing` | Morphing | Float | 0.0–1.0 | 0.0 |

Arp patterns: 0=Up, 1=Down, 2=Up-Down, 3=Down-Up, 4=Random, 5=Cascade
Octave folds: 0=None, 1=1 Oct, 2=Mirror, 3=Harmonic, 4=Inharm

Knob 8 popup label and value change dynamically based on current mode.

---

## Open questions
- None — all design questions resolved as of v0.2.0

---

## Move hardware constraints (never violate)
- Block size: 128 frames at 44100 Hz (~2.9 ms)
- Audio: int16 stereo interleaved
- No heap allocation in render path
- No `printf` / logging in render path
- No FTZ on ARM — compile with `-ffast-math`
- Files on device must be owned by `ableton:users`
- CPU target: < 15% per block

---

## API constraints (audio_fx)

- API: `audio_fx_api_v2_t`, entry: `move_audio_fx_init_v2(const host_api_v1_t *host)`
- `process_block(instance, int16_t *audio_inout, frames)`: in-place stereo int16, 128 frames
- `set_param(instance, key, val)` / `get_param(instance, key, buf, buf_len)`: string-based
- Knob overlay: `knob_N_adjust`, `knob_N_name`, `knob_N_value` (N = 1–8, 1-indexed)
- Knob 8 handlers must check current mode and dispatch to the correct mode-specific param
- MIDI export: `move_audio_fx_on_midi` via `dlsym` (not struct pointer)
- `raw_midi: true` at root level of module.json
- Capabilities: `chainable: true, component_type: "audio_fx", audio_in: true`
- Install path: `modules/audio_fx/structor/`

## Knob overlay implementation (critical)

The Shadow UI does NOT pass knob CCs to ui_chain.js. The framework calls DSP directly:
1. `set_param(instance, "knob_N_adjust", "<delta>")` — change value
2. `get_param(instance, "knob_N_name", ...)` — popup label
3. `get_param(instance, "knob_N_value", ...)` — popup display value

For Knob 8: check `current_mode` in `knob_8_adjust` handler, route delta to the
correct mode-specific parameter, and return the correct label/value in get_param.

Display formats:
- Float params: `%d%%` (percentage)
- Enum params: name string (e.g. "Up-Down", "Mirror")
- Speed curve: `%.1fx` (e.g. "1.5x")

---

## Repo map
- `src/structor.c` — all DSP + Move API wrapper (single file)
- `src/pffft.c`, `src/pffft.h` — PFFFT library (BSD-like, cpuimage/pffft)
- `src/fftpack.c`, `src/fftpack.h` — FFTPACK dependency for pffft
- `module.json` — module metadata (version must match git tag)
- `ui_chain.js` — Shadow UI (jog menu + dynamic Knob 8 + mode subheader)
- `scripts/build.sh` — ARM64 cross-compile (3 source files → single .so)
- `scripts/install.sh` — deploy via SSH + fix ownership
- `Dockerfile` — build container (debian bookworm-slim + aarch64 gcc + dos2unix)
- `.github/workflows/release.yml` — CI: version check, build, tag, release tarball
- `release.json` — auto-updated by CI, never edit manually
- `design-spec.md` — authoritative design record
- `CLAUDE.md` — this file

## Build
```bash
# Always use scripts/build.sh (matches CI exactly)
./scripts/build.sh

# Deploy
./scripts/install.sh
```

Source files: `fftpack.c`, `pffft.c`, `structor.c` (compiled together into single .so).
Compiler flags: `-O2 -shared -fPIC -ffast-math -Wall`

## Release
Use `/schwung-release` when ready.
