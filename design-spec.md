# Structor — Design Spec

> **Status:** v0.2.0 (8 modes, Knob 8 rework)
> **Plugin type:** audio_fx
> **Module ID:** `structor`
> **Last updated:** 2026-03-13

---

## What it is

Structor is a real-time sound deconstructor/reconstructor for Ableton Move.
Audio enters, gets written to a circular buffer (~3 sec). An event detector
identifies onsets, peaks, and zero-crossings. A reconstruction engine then
reassembles detected events into new temporal orders using one of 8 modes —
from random shuffling to pitch-sorted sweeps to density arpeggiators to the
percussive tape-cut effect "Deltarupt" to the spectral-density hybrid "Spec Warp."
Output is grain-based with Hann windowing, Hermite interpolation, wet/dry mix,
and feedback recirculation.

## Sonic intent

**References:** Musique concrete micro-montage (Pierre Schaeffer, Bernard
Parmegiani), acousmatic sound composition, tape splicing, PO-33 style
glitch effects, Unfiltered Audio SpecOps (spectral bin analysis).

**Philosophy:** Deconstruct the temporal structure of sound and rebuild it
algorithmically. Each mode offers a different listening perspective on the
same source material — from chaos (Random) to order (Pitch Sort) to rhythm
(Density Arp) to violence (Deltarupt) to intelligence (Spec Warp).

**Not:** A granular synthesizer (no pitch shifting of grains). Not a delay or
reverb — the buffer is for event detection, not echo. Not a spectral
processor (ZCR is used for rough frequency estimation only).

---

## DSP architecture

**Core algorithms:**
- Circular ring buffer (131,072 samples, ~3 sec stereo @ 44.1 kHz)
- Envelope follower (10 ms attack, 50 ms release) for onset detection
- Zero-crossing rate for spectral center estimation (200 Hz – 8 kHz)
- Event culling: keep top N by amplitude (controlled by Density param)
- Grain-based reconstruction with Hann window + Hermite 4-point interpolation
- Soft limiting via tanh() saturation

**Voice architecture:** N/A (audio effect, not a generator)

**Signal flow:**
```
Audio In (stereo int16)
  → Circular buffer (write + feedback injection)
    → Event detection (onset, peak, zero-crossing)
      → Event culling (top N by amplitude)
        → Playback order (mode-dependent sort/shuffle/arp)
          → Grain synthesis (Hann window, Hermite interp)
            → Mode-specific envelope (Deltarupt: quadratic attack + instant cut)
              → Soft limiting (tanh)
                → Wet/dry mix
                  → Audio Out (stereo int16)
```

**Known DSP challenges:**
- ZCR-based frequency estimation is rough — FFT would be better (Phase 2)
- No denormal guard yet — ARM has no FTZ, needs `-ffast-math`
- Event detection minimum spacing (10 ms) may need tuning per mode

---

## Parameters

### Knobs 1–6: Global (active in all modes)

| # | Name | Key | Type | Range | Default | Notes |
|---|------|-----|------|-------|---------|-------|
| 1 | Detection | `detection` | Float | 0.0–1.0 | 0.1 | Onset sensitivity threshold |
| 2 | Density | `density` | Float | 0.1–1.0 | 0.7 | Fraction of detected events to keep |
| 3 | Grain Size | `grain_size` | Float | 0.5–2.0 | 1.0 | Grain duration multiplier (base: 256 samples) |
| 4 | Time Warp | `time_warp` | Float | 0.5–2.0 | 1.0 | Playback speed variation factor |
| 5 | Mix | `mix` | Float | 0.0–1.0 | 0.5 | Wet/dry balance |
| 6 | Feedback | `feedback` | Float | 0.0–0.95 | 0.2 | Buffer recirculation amount |

### Knob 7: Mode selector

| # | Name | Key | Type | Range | Default | Notes |
|---|------|-----|------|-------|---------|-------|
| 7 | Mode | `mode` | Enum | 0–7 | 0 | Reconstruction mode (see below) |

### Knob 8: Mode-specific parameter (context-sensitive)

| Mode | Knob 8 Name | Key | Type | Range | Default | Description |
|------|-------------|-----|------|-------|---------|-------------|
| 0 — Random Remix | Shfl Bias | `shuffle_bias` | Float | 0.0–1.0 | 0.5 | 0=pure random, 1=constrained nearby swaps |
| 1 — Pitch ↑ | Pitch Win | `pitch_range_window` | Float | 0.0–1.0 | 0.0 | 0=strict sort, 1=coarse frequency quantization |
| 2 — Pitch ↓ | Oct Fold | `octave_fold` | Enum | 0–4 | 0 | None/1 Oct/Mirror/Harmonic/Inharmonic |
| 3 — Density ↑ | Dens Crv | `density_curve` | Float | 0.0–1.0 | 0.0 | 0=linear crescendo, 0.5=sigmoid, 1=exponential |
| 4 — Time Warp | Spd Curve | `speed_curve_exp` | Float | 0.5–2.0 | 1.0 | Amplitude-to-speed mapping exponent |
| 5 — Density Arp | Arp Ptrn | `arp_pattern` | Enum | 0–5 | 0 | Up/Down/Up-Down/Down-Up/Random/Cascade |
| 6 — Deltarupt | Attack | `deltarupt_attack` | Float | 0.0–1.0 | 0.05 | Envelope rise time (0=click, 1=full bloom) |
| 7 — Spec Warp | Morphing | `density_morphing` | Float | 0.0–1.0 | 0.0 | 0=pure pitch sort, 1=pure density sort |

---

## 8 Reconstruction modes

| Mode | Name | Algorithm | Knob 8 | Sound character |
|------|------|-----------|--------|-----------------|
| 0 | Random Remix | Bias-controlled shuffle | Shfl Bias | Unpredictable → constrained montage |
| 1 | Pitch Sort ↑ | Freq quantization + ascending sort | Pitch Win | Strict → chunky pitch sweep |
| 2 | Pitch Sort ↓ | Octave-folded descending sort | Oct Fold | Raw freq → harmonic/inharmonic |
| 3 | Density ↑ | Amplitude sort + curve shaping | Dens Crv | Linear → sigmoid → exponential crescendo |
| 4 | Time Warp | Original order, amplitude^exp speed | Spd Curve | Gentle → extreme speed variation |
| 5 | Density Arp | Density-sorted with cycling pattern | Arp Ptrn | 6 rhythmic density patterns |
| 6 | Deltarupt | Density-sorted, quadratic attack + cut | Attack | Tape cut / reverse percussion |
| 7 | Spec Warp | lerp(freq, amplitude, morph) sort | Morphing | Pitch-sorting ↔ density-sorting |

### Arp Pattern sub-modes (Mode 5, Knob 8)

| Value | Pattern | Effect |
|-------|---------|--------|
| 0 | Up | Quiet → Loud (building intensity) |
| 1 | Down | Loud → Quiet (releasing energy) |
| 2 | Up-Down | Q→L→Q zigzag (breathing motion) |
| 3 | Down-Up | L→Q→L reverse (pulse/heartbeat) |
| 4 | Random | Shuffled density order (chaos) |
| 5 | Cascade | Jump to loudest, then descend (waterfall) |

### Octave Fold sub-modes (Mode 2, Knob 8)

| Value | Name | Effect |
|-------|------|--------|
| 0 | None | Raw frequency descending sort |
| 1 | 1 Oct | Fold all to single octave, sort by pitch class |
| 2 | Mirror | Reflect around octave boundaries |
| 3 | Harmonic | Collapse to fundamental harmonic series |
| 4 | Inharm | Golden-ratio inharmonic spread |

### Deltarupt envelope detail (Mode 6)

- Attack phase: quadratic curve `window = phase²` (smooth, organic rise)
- Sustain: none — instant cutoff at end of attack phase
- Attack = 0.0: percussive click (tape splice)
- Attack = 0.3: reverse cymbal swell
- Attack = 0.6: blooming ambient texture
- Attack = 1.0: full grain duration is the attack (extreme bloom)

### Spectral Density Warp detail (Mode 7)

- Normalizes frequency and amplitude to [0,1] across all detected events
- Sort key = lerp(freq_normalized, amp_normalized, morphing)
- Morphing = 0.0: behaves like Mode 1 (pitch ascending)
- Morphing = 0.5: hybrid frequency-density landscape
- Morphing = 1.0: behaves like Mode 3 (density ascending)

---

## Open questions

- [x] ~~Multi-page knob layout~~ — Not needed. 8 knobs on 1 page covers all params.
- [x] ~~Jog wheel~~ — Standard behavior (cycles through parameters in the menu).
- [x] ~~FFT-based spectral analysis~~ — Implemented with pffft (256-point real FFT + parabolic interpolation).
- [x] ~~ui_chain.js~~ — Implemented: dynamic Knob 8 label/value/range per mode, mode subheader, jog menu.
- [x] ~~Preset system~~ — Not planned.

---

## Hardware constraints (Move)

- Block size: 128 frames at 44100 Hz (~2.9 ms)
- Audio format: int16 stereo interleaved
- No FTZ on ARM — guard against denormals (`-ffast-math`)
- No heap allocation in render path
- No `printf` or logging in render path
- CPU target: < 15% per block
- Memory budget: ~1.5 MB (circular buffer dominates)
- Files on device must be owned by `ableton:users`

---

## Design conversation reference

> "I'd like to create a sound reorganizer/deconstructor/reconstructor effect
> from open-source DSP, inspired by micro-montage in electroacoustic music
> (musique concrete). It's intended to work on the Move-Everything framework."
>
> — Original design prompt, 2025-03-06
>
> v0.2.0 expansion: Knob 8 rework (unique parameter per mode) + Mode 7
> (Spectral Density Warp). Inspired by Unfiltered Audio SpecOps spectral
> bin analysis and experimental FX techniques.
>
> — Design conversation, 2026-03-12
