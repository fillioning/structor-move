# Structor — Design Spec

> **Status:** Pre-implementation (prototype code exists, needs full rewrite to Move Everything API)
> **Plugin type:** audio_fx
> **Module ID:** `move-everything-structor`
> **Last updated:** 2025-03-12

---

## What it is

Structor is a real-time sound deconstructor/reconstructor for Ableton Move.
Audio enters, gets written to a circular buffer (~3 sec). An event detector
identifies onsets, peaks, and zero-crossings. A reconstruction engine then
reassembles detected events into new temporal orders using one of 7 modes —
from random shuffling to pitch-sorted sweeps to density arpeggiators to the
percussive tape-cut effect "Deltarupt." Output is grain-based with Hann
windowing, Hermite interpolation, wet/dry mix, and feedback recirculation.

## Sonic intent

**References:** Musique concrete micro-montage (Pierre Schaeffer, Bernard
Parmegiani), acousmatic sound composition, tape splicing, PO-33 style
glitch effects.

**Philosophy:** Deconstruct the temporal structure of sound and rebuild it
algorithmically. Each mode offers a different listening perspective on the
same source material — from chaos (Random) to order (Pitch Sort) to rhythm
(Density Arp) to violence (Deltarupt).

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
- Current prototype does `malloc`/`free` per block — must pre-allocate
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
| 7 | Mode | `mode` | Enum | 0–6 | 0 | Reconstruction mode (see below) |

### Knob 8: Mode-specific parameter (context-sensitive)

| Mode | Knob 8 Name | Key | Type | Range | Default | Description |
|------|-------------|-----|------|-------|---------|-------------|
| 0 — Random Remix | Reshuffle | `reshuffle` | Float | 0.0–1.0 | 0.5 | Re-randomization rate (0=static shuffle, 1=constant chaos) |
| 1 — Pitch ↑ | Scatter | `scatter` | Float | 0.0–1.0 | 0.0 | Sort fidelity (0=strict ascending, 1=noisy/loose sort) |
| 2 — Pitch ↓ | Scatter | `scatter` | Float | 0.0–1.0 | 0.0 | Sort fidelity (0=strict descending, 1=noisy/loose sort) |
| 3 — Density ↑ | Curve | `curve` | Float | 0.0–1.0 | 0.0 | Gradient shape (0=linear, 0.5=logarithmic, 1=exponential) |
| 4 — Time Warp | Drift | `drift` | Float | 0.0–1.0 | 0.3 | Speed deviation range (0=subtle variation, 1=extreme warping) |
| 5 — Density Arp | Arp Pattern | `arp_pattern` | Enum | 0–5 | 0 | Up / Down / Up-Down / Down-Up / Random / Cascade |
| 6 — Deltarupt | Attack | `deltarupt_attack` | Float | 0.0–1.0 | 0.05 | Envelope rise time (0=percussive click, 1=full-grain bloom) |

---

## 7 Reconstruction modes

| Mode | Name | Algorithm | Knob 8 | Sound character |
|------|------|-----------|--------|-----------------|
| 0 | Random Remix | Fisher-Yates shuffle | Reshuffle | Unpredictable, chaotic montage |
| 1 | Pitch Sort ↑ | qsort by frequency ascending | Scatter | Low-to-high frequency sweep |
| 2 | Pitch Sort ↓ | qsort by frequency descending | Scatter | High-to-low frequency sweep |
| 3 | Density ↑ | qsort by amplitude ascending | Curve | Quiet-to-loud crescendo |
| 4 | Time Warp | Original order, variable speed | Drift | Rhythmic distortion/stretch |
| 5 | Density Arp | Density-sorted with cycling pattern | Arp Pattern | Cascading rhythmic density |
| 6 | Deltarupt | Density-sorted, exponential attack + instant cut | Attack | Tape cut / reverse percussion |

### Arp Pattern sub-modes (Mode 5, Knob 8)

| Value | Pattern | Effect |
|-------|---------|--------|
| 0 | Up | Quiet → Loud (building intensity) |
| 1 | Down | Loud → Quiet (releasing energy) |
| 2 | Up-Down | Q→L→Q zigzag (breathing motion) |
| 3 | Down-Up | L→Q→L reverse (pulse/heartbeat) |
| 4 | Random | Shuffled density order (chaos) |
| 5 | Cascade | Jump to loudest, then descend (waterfall) |

### Deltarupt envelope detail (Mode 6)

- Attack phase: quadratic curve `window = phase²` (smooth, organic rise)
- Sustain: none — instant cutoff at end of attack phase
- Attack = 0.0: percussive click (tape splice)
- Attack = 0.3: reverse cymbal swell
- Attack = 0.6: blooming ambient texture
- Attack = 1.0: full grain duration is the attack (extreme bloom)

---

## Open questions

- [ ] Multi-page knob layout? Current design uses 1 page × 8 knobs. Boris uses 4 pages × 8 knobs — could expand later with advanced params (e.g. event spacing, window shape, spectral resolution).
- [ ] Jog wheel: currently unused. Could map to mode selection (freeing Knob 7) or grain scrubbing.
- [ ] FFT-based spectral analysis: Phase 2 improvement over ZCR — scope and priority TBD.
- [ ] ui_chain.js: not yet designed — needed for Shadow UI integration.
- [ ] Preset system: save/recall parameter snapshots? Deferred to post-MVP.

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
