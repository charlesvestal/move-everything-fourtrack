# Four Track

A 4-track audio recorder module for Move Anything on Ableton Move.

## Features

- **4 Independent Audio Tracks**: Record up to 5 minutes per track
- **Signal Chain Integration**: Load any Signal Chain patch as your sound source
- **Live Monitoring**: Play your synth while selecting tracks, record when ready
- **Count-In Recording**: Optional 4-beat count-in before recording starts
- **Punch-In/Out**: Start or stop recording while playback continues
- **Metronome**: Beat-aligned click track for timing
- **Full Mixer**: Per-track level and pan controls
- **Knob Macros**: Synth parameters mapped to hardware knobs with overlays

## Installation

### Build from Source

```bash
./scripts/build.sh      # Builds for ARM64 via Docker
./scripts/install.sh    # Deploys to connected Move
```

### Requirements

- Move Anything installed on Move device
- Docker (for cross-compilation)
- SSH access to Move device

## Usage

1. Select Four Track from the Move Anything menu
2. Tap a track row to open the patch browser
3. Select a Signal Chain patch (or "(None)" to clear)
4. Arm the track with the Sample button (turns red)
5. Enable record mode with the Record button (turns red)
6. Press Play to start recording (turns green)
7. Stop, switch tracks, and layer more parts

See [docs/getting-started.md](docs/getting-started.md) for a detailed tutorial.

## Documentation

- [Getting Started Guide](docs/getting-started.md) - Quick tutorial for first-time users
- [User Manual](docs/manual.md) - Complete reference documentation
- [Test Sequence](docs/test-sequence.md) - QA test procedures

## Controls

| Control | Action |
|---------|--------|
| Track Rows | Select track (tap selected track for patch browser) |
| Play | Start/stop playback (white=stopped, green=playing) |
| Record | Toggle record mode (white=off, red=armed) |
| Sample | Arm/disarm selected track for recording |
| Menu | Toggle Main/Mixer views |
| Back | Return to previous view / exit module |
| Left | Jump to start of track content |
| Right | Jump to end of track content |
| Knobs 1-8 | Synth macro controls (in Main view) |
| Knobs 1-4 | Track levels (in Mixer view) |
| Knobs 5-8 | Track pan (in Mixer view) |
| Master | Selected track level |
| Step 1-4 | Mute tracks |
| Shift+Step | Solo tracks |

## Views

### Main View
```
Four Track        [-] 0:00.0
  T1: Pad Strings    L:80 P:C00
> T2: Bass Deep      L:75 P:L25
R T3: Lead Synth     L:90 P:R10
  T4: Empty          L:80 P:C00
```
- `>` indicates selected track
- `R` indicates armed track
- Level (L:00-99) and Pan (P:L50-C00-R50) shown for each track

### Mixer View
Visual faders for all 4 tracks with pan position ticks.

### Patch Browser
Jog wheel to scroll, click to load. "(None)" clears the patch.

## Architecture

```
src/
  module.json        # Module metadata
  ui.js              # JavaScript UI (views, controls)
  dsp/
    fourtrack.c      # C DSP plugin (recording, playback, mixing)
    plugin_api_v1.h  # Move Anything plugin API
```

## Technical Specs

- Sample Rate: 44,100 Hz
- Bit Depth: 16-bit stereo
- Max Recording: 300 seconds (5 minutes) per track
- Memory Usage: ~212 MB for all 4 tracks

## License

MIT License - See LICENSE file

## Contributing

Part of the Move Anything project. Contributions welcome!
