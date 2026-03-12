# Structor — Claude Code context

## What this is
Musique concrete & acousmatique-inspired real-time sound deconstructor/reconstructor for Ableton Move.

Plugin type: audio_fx
Module ID: `move-everything-structor`
API: `audio_fx_api_v2_t`
Entry: `move_audio_fx_init_v2(const host_api_v1_t *host)`
Language: C

Read `design-spec.md` for full design intent. This file is the compressed version.

---

## Sonic intent
Inspired by micro-montage techniques from musique concrete (Schaeffer, Parmegiani).
Deconstructs audio into detected events (onsets, peaks, zero-crossings), then
reconstructs them in 7 algorithmic modes — from random chaos to pitch-sorted sweeps
to density arpeggiators to the tape-cut "Deltarupt" effect. Not a granular synth,
not a delay, not a spectral processor.

---

## DSP architecture
Audio enters a circular ring buffer (~3 sec, 131072 samples stereo @ 44.1 kHz).
An envelope follower + zero-crossing detector identifies events (onsets, peaks, ZC).
Events are culled by amplitude (Density param). A mode-dependent algorithm reorders
events (shuffle, sort, arp cycle). Grain-based synthesis reads back events with Hann
windowing and Hermite interpolation. Deltarupt mode uses a quadratic attack envelope
with instant cutoff. Output passes through tanh soft limiting and wet/dry mixing.
Feedback recirculates output back into the buffer.

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
| 7 | `mode` | Mode | Enum | 0–6 | 0 | 1 |

Modes: 0=Random Remix, 1=Pitch ↑, 2=Pitch ↓, 3=Density ↑, 4=Time Warp, 5=Density Arp, 6=Deltarupt

### Knob 8: Mode-specific (context-sensitive)

| Mode | Key | Label | Type | Range | Default | Step |
|------|-----|-------|------|-------|---------|------|
| 0 — Random | `reshuffle` | Reshuffle | Float | 0.0–1.0 | 0.5 | 0.01 |
| 1 — Pitch ↑ | `scatter` | Scatter | Float | 0.0–1.0 | 0.0 | 0.01 |
| 2 — Pitch ↓ | `scatter` | Scatter | Float | 0.0–1.0 | 0.0 | 0.01 |
| 3 — Density ↑ | `curve` | Curve | Float | 0.0–1.0 | 0.0 | 0.01 |
| 4 — Time Warp | `drift` | Drift | Float | 0.0–1.0 | 0.3 | 0.01 |
| 5 — Density Arp | `arp_pattern` | Arp Ptrn | Enum | 0–5 | 0 | 1 |
| 6 — Deltarupt | `deltarupt_attack` | Attack | Float | 0.0–1.0 | 0.05 | 0.01 |

Arp patterns: 0=Up, 1=Down, 2=Up-Down, 3=Down-Up, 4=Random, 5=Cascade

Knob 8 popup label and value change dynamically based on current mode.

---

## Open questions (resolve before implementing the relevant section)
- Multi-page knob layout (currently 1 page × 8) — expand later?
- Jog wheel assignment (currently unused)
- FFT-based spectral analysis as Phase 2 ZCR replacement
- ui_chain.js design for Shadow UI integration
- Preset system — deferred to post-MVP

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
- Install path: `modules/audio_fx/move-everything-structor/`

## Knob overlay implementation (critical)

The Shadow UI does NOT pass knob CCs to ui_chain.js. The framework calls DSP directly:
1. `set_param(instance, "knob_N_adjust", "<delta>")` — change value
2. `get_param(instance, "knob_N_name", ...)` — popup label
3. `get_param(instance, "knob_N_value", ...)` — popup display value

For Knob 8: check `current_mode` in `knob_8_adjust` handler, route delta to the
correct mode-specific parameter, and return the correct label/value in get_param.

Use `%d%%` for percentage display, `%s` for enum names. Never `%.2f`.

---

## Repo map
- `src/structor.c` — all DSP + Move API wrapper (single file, ~600 lines)
- `module.json` — module metadata (version must match git tag)
- `ui_chain.js` — Shadow UI (jog menu + knob handling, Clouds-style dynamic subheader)
- `scripts/build.sh` — Docker ARM64 cross-compile
- `scripts/install.sh` — deploy via SSH + fix ownership
- `Dockerfile` — build container (debian bookworm-slim + aarch64 gcc + dos2unix)
- `.github/workflows/release.yml` — CI: version check, build, tag, release tarball
- `release.json` — auto-updated by CI, never edit manually
- `design-spec.md` — authoritative design record
- `CLAUDE.md` — this file

## Build
```bash
# Docker (preferred — no local compiler needed)
docker build -t structor-builder .
docker cp $(docker create structor-builder):/build/dist/move-everything-structor ./dist/

# Or with local cross-compiler
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh

# Deploy
./scripts/install.sh [move-ip]
```

## Release
Use `/move-anything-release` when ready.
