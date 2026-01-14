# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

Four Track is a 4-track audio recorder module for Move Anything that integrates with Signal Chain patches for sound generation.

## Architecture

```
src/
  dsp/
    fourtrack.c       # Main DSP implementation
    plugin_api_v1.h   # Plugin API types (from move-anything)
  ui.js               # JavaScript UI module
  module.json         # Module metadata
docs/
  manual.md           # User manual
  test-sequence.md    # QA test procedures
  getting-started.md  # Quick start tutorial
```

## Key Implementation Details

### DSP Plugin (fourtrack.c)

Implements Move Anything plugin_api_v1:
- `on_load`: Initialize 4 track buffers (60s each), scan patches
- `on_unload`: Free buffers, unload synth
- `on_midi`: Forward MIDI to loaded synth
- `set_param`: Track control, transport, patch loading
- `get_param`: Track state queries
- `render_block`: Mix all tracks, record armed track

### Track System

Each track has:
- 60 second stereo buffer (malloc'd on load)
- Associated chain patch name
- Level, pan, mute, solo controls
- Recording length in samples

### Transport

Four states:
- `TRANSPORT_STOPPED`: No playback
- `TRANSPORT_PLAYING`: All tracks play back
- `TRANSPORT_RECORDING`: Armed track records, others play
- `TRANSPORT_COUNTIN`: 4-beat count-in before recording starts

### Metronome and Count-In

- Metronome generates beat-aligned clicks during playback/recording
- Count-in provides 4 beats before recording starts (when enabled)
- Beat position calculated from playhead: `(playhead + sample) % samples_per_beat`
- Punch-in/out supported: can toggle recording while playing

### Signal Chain Integration

- Dynamically loads synth modules via dlopen
- Uses same subplugin host API as chain_host.c
- Only one synth active at a time (for armed track)
- Records synth output to track buffer

### UI Module (ui.js)

Three views:
- Main: Track list with status, transport display
- Patch: Chain patch browser with jog navigation
- Mixer: Visual faders for level/pan

Uses shared utilities from move-anything:
- menu_layout.mjs for headers/footers/lists
- constants.mjs for MIDI CC mappings
- input_filter.mjs for capacitive touch filtering

## Build Commands

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move device
```

## Key Constants

Audio:
- SAMPLE_RATE: 44100 Hz
- FRAMES_PER_BLOCK: 128
- MAX_RECORD_SECONDS: 60
- NUM_TRACKS: 4

MIDI:
- Track rows: CC 40-43
- Knobs 1-8: CC 71-78
- Master: CC 79
- Play: CC 85
- Record: CC 118

## Dependencies

- move-anything host runtime
- Signal Chain module (for patches)
- Docker (for cross-compilation)
