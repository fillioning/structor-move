# Structor

Musique concrete-inspired sound deconstructor/reconstructor for Ableton Move.

An audio FX plugin for the [Move Everything](https://github.com/charlesvestal/move-everything) framework. Deconstructs audio into detected events (onsets, peaks, zero-crossings) and reconstructs them in 8 algorithmic modes with per-grain LP/HP filtering, randomization, and a sequencer for random parameters.

Original DSP — envelope follower + FFT spectral analysis (PFFFT) + grain-based reconstruction.

## Install

Download the latest release from the [Releases page](https://github.com/fillioning/move-everything-structor/releases) and extract to your Move.

## Modes

| # | Mode | Special knob | Character |
|---|------|-------------|-----------|
| 0 | Random | Shuffle Bias | Chaotic montage |
| 1 | Pitch Up | Pitch Window | Low-to-high frequency sweep |
| 2 | Pitch Down | Octave Fold | High-to-low with harmonic folding |
| 3 | Density Up | Density Curve | Quiet-to-loud crescendo |
| 4 | Time Warp | Speed Curve | Amplitude-driven speed variation |
| 5 | Dens Arp | Arp Pattern | Rhythmic density cycling |
| 6 | Deltarupt | Attack | Tape cut / reverse percussion |
| 7 | Spec Warp | Morphing | Pitch-density hybrid sort |

## Parameters

### Structor page (Knobs 1-8)
- **Knob 1:** Envelope (grain window: rectangle to Hann to narrow)
- **Knob 2:** Density (event culling <1, grain overlap >1)
- **Knob 3:** Grain Size (duration multiplier)
- **Knob 4:** Time Warp (playback speed)
- **Knob 5:** Feedback (buffer recirculation)
- **Knob 6:** Mode (8 reconstruction modes)
- **Knob 7:** Special (context-sensitive per mode)
- **Knob 8:** Mix (dry/wet)

### Randomize page (Knobs 1-8)
- **Rnd Env / Density / Grain / Time:** Per-grain random offsets
- **Rnd Pan:** Random stereo panning per grain
- **Rnd Filter:** Isolator3 DJ LP/HP filter per grain (3-stage cascade)
- **Sequence:** Timed random changes (On/Off, Time, Multiplier)
- **Detection:** Onset sensitivity

## Build

```bash
# Docker (preferred)
docker build -t structor-build .
docker cp $(docker create structor-build):/build/dist/structor ./dist/

# Or with local cross-compiler
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

## Deploy

```bash
./scripts/install.sh [move-ip]
```

## Credits

- DSP: Original (fillioning)
- FFT: [PFFFT](https://bitbucket.org/jpommier/pffft) (BSD-like)
- Isolator3 filter: Inspired by Octocosme DJ filter (fillioning)
- Framework: [Move Everything](https://github.com/charlesvestal/move-anything)

## License

MIT
