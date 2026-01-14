# Four Track Recorder - User Manual

## Overview

Four Track is a 4-track audio recorder module for Move Anything. It allows you to:

- Record up to 4 independent audio tracks
- Associate each track with a Signal Chain patch for integrated sound generation
- Play back all tracks simultaneously while recording new ones
- Mix tracks with level and pan controls
- Use a metronome for timing
- Loop playback for practice and arrangement

## Concepts

### Tracks

Four Track provides 4 independent audio tracks. Each track can:

- Store up to 60 seconds of stereo audio at 44.1kHz
- Be associated with a Signal Chain patch (synth + effects)
- Have its own level, pan, mute, and solo controls
- Be armed for recording independently

### Transport

The transport controls manage playback and recording:

- **Stopped**: No playback, playhead at current position
- **Playing**: All tracks play back simultaneously
- **Recording**: Armed track records while others play back (overdub mode)
- **Count-In**: 4-beat count-in before recording starts (when enabled)

### Punch-In/Out

You can start or stop recording while playback continues:
- While playing, press Record to punch in (start recording armed track)
- While recording, press Record to punch out (stop recording, continue playback)
- This allows seamless overdubbing without stopping the transport

### Signal Chain Integration

When you assign a Signal Chain patch to a track, that patch's synth and effects are used when recording that track. This allows you to:

1. Select a track
2. Load a chain patch (e.g., "DX7 Piano + Reverb")
3. Arm the track for recording
4. Play and record your performance
5. Switch to another track with a different patch

## Controls

### Main View

The main view shows all 4 tracks with their status.

**Track Selection:**
- Track Row Buttons (CC 40-43): Select track 1-4
- Up/Down Arrows: Navigate between tracks
- Jog Wheel: Scroll through tracks

**Transport:**
- Play Button: Start/stop playback
- Record Button: Toggle recording (requires armed track)
- Shift + Record: Arm/disarm selected track

**Track Functions (with Shift held):**
- Shift + Track Row: Arm track for recording
- Step 1-4: Mute track 1-4
- Shift + Step 1-4: Solo track 1-4

**Tempo:**
- Left/Right Arrows: Decrease/increase tempo by 1 BPM
- Shift + Left/Right: Decrease/increase tempo by 10 BPM

### Patch View

Access by pressing Menu from the main view.

- Jog Wheel: Navigate patch list
- Jog Click: Load selected patch to current track
- Back: Return to main view

### Mixer View

Access by pressing Menu twice from the main view.

**Level Controls:**
- Knobs 1-4: Adjust level for tracks 1-4
- Master Knob: Adjust level for selected track

**Pan Controls:**
- Knobs 5-8: Adjust pan for tracks 1-4

### LED Indicators

**Track Row LEDs:**
- Red: Track is armed for recording
- Track color: Track is selected
- Grey: Track has recorded audio
- Off: Track is empty and not selected

**Play LED:**
- Green: Playing or recording
- Off: Stopped

**Record LED:**
- Red: Currently recording
- White: A track is armed
- Off: No track armed

## Parameters

### Track Parameters

Each track has the following adjustable parameters:

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| Level | 0-100% | 80% | Track volume |
| Pan | L100-R100 | Center | Stereo position |
| Mute | On/Off | Off | Silence track |
| Solo | On/Off | Off | Solo track (mutes others) |

### Transport Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| Tempo | 20-300 BPM | 120 | Metronome and timing |
| Metronome | On/Off | Off | Audible click track |
| Count-In | On/Off | Off | 4-beat count-in before recording |
| Loop | On/Off | Off | Loop playback |

## Workflow Examples

### Basic Recording Session

1. Select Track 1
2. Press Menu to open patch browser
3. Select a synth patch and press Jog to load
4. Press Shift + Record to arm Track 1 (LED turns red)
5. Press Record to start recording
6. Play your performance
7. Press Record again to stop recording
8. Press Play to review your take

### Overdubbing

1. Record Track 1 as above
2. Select Track 2
3. Load a different patch for Track 2
4. Arm Track 2 for recording
5. Press Record - Track 1 plays back while Track 2 records
6. Play your performance along with Track 1
7. Repeat for Tracks 3 and 4

### Mixing

1. Open Mixer view (Menu twice)
2. Use Knobs 1-4 to balance track levels
3. Use Knobs 5-8 to position tracks in stereo field
4. Use Step buttons to mute/solo tracks as needed

## Technical Specifications

- Sample Rate: 44,100 Hz
- Bit Depth: 16-bit
- Channels: Stereo per track
- Maximum Recording Time: 300 seconds (5 minutes) per track
- Total Tracks: 4
- Simultaneous Playback: All 4 tracks
- Simultaneous Recording: 1 track (overdub mode)
- Memory Usage: ~53 MB per track (~212 MB for all 4 tracks)

## Troubleshooting

### No Sound

- Check that a track with audio is not muted
- Verify track levels are above 0
- If solo is enabled, make sure the playing track is soloed

### Can't Record

- Ensure a track is armed (Record LED should be white)
- Check that the armed track has a loaded patch
- Verify transport is in Record mode (Record LED should be red)

### Patches Not Found

- Ensure Signal Chain module is installed
- Check that patches exist in `/data/UserData/move-anything/modules/chain/patches/`
- Use Shift+Menu to rescan patches

## Version History

- v0.1.0: Initial release
  - 4 audio tracks with 5 minutes each
  - Signal Chain patch integration
  - Transport controls with count-in
  - Punch-in/out recording while playing
  - Beat-aligned metronome
  - Mixer with level and pan
  - Loop playback
