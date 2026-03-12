# Structor

Musique concrète sound deconstructor/reconstructor for Ableton Move.

An audio FX plugin for the [Move-Anything](https://github.com/charlesvestal/move-anything) framework that deconstructs audio into detected events (onsets, peaks, zero-crossings) and reconstructs them in 7 algorithmic modes.

## Modes

| # | Mode | Character |
|---|------|-----------|
| 0 | Random Remix | Unpredictable, chaotic montage |
| 1 | Pitch ↑ | Low-to-high frequency sweep |
| 2 | Pitch ↓ | High-to-low frequency sweep |
| 3 | Density ↑ | Quiet-to-loud crescendo |
| 4 | Time Warp | Rhythmic distortion/stretch |
| 5 | Density Arp | Cascading rhythmic density |
| 6 | Deltarupt | Tape cut / reverse percussion |

## Parameters

- **Knobs 1-6:** Detection, Density, Grain Size, Time Warp, Mix, Feedback
- **Knob 7:** Mode selector (0-6)
- **Knob 8:** Mode-specific parameter (changes label per mode)

## Build

```bash
# Docker (preferred — no local compiler needed)
docker build -t structor-builder .
docker cp $(docker create structor-builder):/build/dist/move-everything-structor ./dist/

# Or with local cross-compiler
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

## Install

```bash
./scripts/install.sh [move-ip]
```

## License

MIT
