# Structor

Musique concrete-inspired sound deconstructor/reconstructor for [Ableton Move](https://www.ableton.com/move/),
built for the [Schwung](https://github.com/charlesvestal/schwung) open-source plugin framework.

Original DSP — envelope follower + FFT spectral analysis (PFFFT) + grain-based reconstruction
with Schroeder reverb, master DJ filter, and 20 presets.

## Modes

| # | Mode | Special Knob | Character |
|---|------|-------------|-----------|
| 0 | Random | Shuffle Bias | Chaotic micro-montage |
| 1 | Pitch Up | Pitch Window | Low-to-high frequency sweep |
| 2 | Pitch Down | Octave Fold | High-to-low with harmonic folding |
| 3 | Density Up | Density Curve | Quiet-to-loud crescendo |
| 4 | Time Warp | Speed Curve | Amplitude-driven speed variation |
| 5 | Dens Arp | Arp Pattern | Rhythmic density cycling |
| 6 | Deltarupt | Attack | Tape cut / reverse percussion |
| 7 | Spec Warp | Morphing | Pitch-density hybrid sort |

## Parameters

### Page 1: Structor (Knobs 1-8)

| Knob | Parameter | Function |
|------|-----------|----------|
| 1 | Envelope | Grain window shape: rectangle → Hann → narrow peak |
| 2 | Density | Event culling (<1) or grain overlap (>1) |
| 3 | Grain Size | Duration multiplier (0.1x–40x) |
| 4 | Time Warp | Playback speed (0.25x–8x) |
| 5 | Feedback | Buffer recirculation (0–95%) |
| 6 | Mode | 8 reconstruction modes |
| 7 | Special | Context-sensitive per mode |
| 8 | Mix | Dry/wet crossfade |

### Page 2: Randomize (Knobs 1-8)

| Knob | Parameter | Function |
|------|-----------|----------|
| 1 | Rnd Env | Per-grain random envelope offset |
| 2 | Rnd Density | Per-grain random density offset |
| 3 | Rnd Grain | Per-grain random grain size offset |
| 4 | Rnd Time | Per-grain random time warp (octave quantized) |
| 5 | Rnd Pan | Random stereo panning per grain |
| 6 | Sequence | Timed random changes (On/Off) |
| 7 | Seq Time | Sequencer period (10–1000 ms) |
| 8 | Seq Mult | Sequencer rate multiplier (1/8x–4x) |

Menu: Detection (onset sensitivity), Rnd Filter (per-grain LP/HP chance)

### Page 3: Presets (Knobs 1-8)

| Knob | Parameter | Function |
|------|-----------|----------|
| 1 | Preset | Cycle through 20 presets (applied in real time) |
| 2 | Rnd Preset | Randomize all parameters (turn to trigger) |
| 3 | Filter | Master DJ LP/HP filter (center=bypass) |
| 4 | Rnd Reverb | % chance per grain to send into reverb |
| 5 | Rev Mix | Reverb wet/dry mix |
| 6 | Rev Size | Room size (tight ambiance → cavernous space) |
| 7 | Rev Decay | Reverb tail length |
| 8 | Rev Damp | High-frequency damping (bright → very dark) |

### Presets

| # | Name | Character |
|---|------|-----------|
| 0 | Init | Default state — neutral starting point |
| 1 | Scatter | Random mode, medium reverb, loose grains |
| 2 | Ascend | Pitch Up sweep, slow time warp, reverbed |
| 3 | Descend | Pitch Down with reverb |
| 4 | Pulse | Density Arp, tight room reverb |
| 5 | Tape Cut | Deltarupt, LP filtered, dry |
| 6 | Spectral | Spec Warp, high reverb, morphing |
| 7 | Ambient | Slow, large reverb, dark |
| 8 | Chaos | High density, fast, random reverb sends |
| 9 | Dark Hall | Full reverb, dark damping, space |
| 10 | Frozen | Very long grains, slow, reverbed pad |
| 11 | Glitch | Tiny grains, fast time warp, stutter |
| 12 | Drone | Huge grains, max feedback, dark space |
| 13 | Sequenced | Density Arp, rhythmic reverb sends |
| 14 | Shatter | Deltarupt, high density, micro cuts |
| 15 | Rise | Pitch Up, long grains, reverbed |
| 16 | Thick | Dense, Density Up, moderate reverb |
| 17 | Lo-Fi | LP filtered, long grains, feedback |
| 18 | Cathedral | Pitch Down, large cathedral reverb |
| 19 | Warp | Time Warp mode, fast, tight reverb |

## v0.2.0 Changelog

### New Features
- **Page 3: Presets** — new UI page with 8 knobs
- **20 presets** with real-time switching (Preset knob cycles 0-19)
- **Rnd Preset** — randomize all parameters with a single knob turn
- **Master DJ filter** — Isolator3 3-stage LP/HP cascade on final output with 10ms per-sample smoothing
- **Schroeder reverb engine** — 4 parallel comb filters + 2 series allpass, per-grain stochastic send
  - Size (delay scaling), Decay (feedback 0.5–0.88), Damping (LP in feedback), Rnd Reverb (% chance per grain)
- **DC-blocking filter** on reconstruction output (removes thuds from buffer position jumps)

### Improvements
- **Anti-click grain overlap** — 2-slot system with 128-sample equal-power cosine crossfade between grains
- **CPU optimization** — replaced per-sample `tanhf`, `sinf`, `powf` with Padé/parabolic fast approximations (~5-8% CPU reduction)
- **Time Warp range** extended to 0.25x–8.0x (was 4.0x)
- **Preset 0 "Init"** matches original Structor defaults exactly

### Bug Fixes
- Crossfade grain window fix: previous grain now uses fade-out ramp only (Hann was zeroing the crossfade)
- Reverb stability: max feedback reduced from 0.97 to 0.88, hard clip inside feedback loop, input attenuation
- Filter excluded from Rnd Preset to prevent gain/distortion artifacts

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
./scripts/install.sh
```

Or install via the Module Store in Schwung.

## Credits

- DSP: Original (fillioning)
- FFT: [PFFFT](https://bitbucket.org/jpommier/pffft) (BSD-like)
- Isolator3 filter: Based on [Airwindows Isolator3](https://github.com/airwindows/airwindows) by Chris Johnson (MIT)
- Framework: [Schwung](https://github.com/charlesvestal/schwung)

## License

MIT
