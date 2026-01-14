# Four Track Recorder - Getting Started Guide

Welcome to Four Track! This guide will help you create your first multi-track recording in just a few minutes.

## What is Four Track?

Four Track is a 4-track audio recorder for your Ableton Move running Move Anything. Think of it like a classic cassette 4-track recorder, but integrated with Signal Chain patches so you can use any of your synths and effects.

With Four Track, you can:
- Layer up to 4 different instrument tracks
- Record up to 5 minutes per track
- Use different synth patches on each track
- Play back your recordings while adding new parts
- Use a metronome and count-in for tight timing
- Punch in/out to fix mistakes without stopping
- Mix and balance your tracks with levels and panning

## Before You Begin

Make sure you have:
1. Move Anything installed on your Move
2. The Four Track module installed
3. The Signal Chain module with some patches (for sound sources)

## Your First Recording

### Step 1: Launch Four Track

1. Enter Move Anything on your Move
2. Select **Four Track** from the menu
3. You'll see the main view with 4 empty tracks

```
Four Track        [-] 0:00.0
> T1: Empty          L:80 P:C00
  T2: Empty          L:80 P:C00
  T3: Empty          L:80 P:C00
  T4: Empty          L:80 P:C00
```

### Step 2: Load a Sound

1. Track 1 should already be selected (highlighted, green LED)
2. **Tap the Track 1 button again** to open the patch browser
3. Use the **Jog wheel** to scroll through available patches
4. "(None)" is shown first - scroll down to see your Signal Chain patches
5. Press the **Jog button** to load a patch
6. You'll return to the main view automatically

Track 1 now shows your loaded patch name:
```
> T1: DX7 Piano      L:80 P:C00
```

**Tip:** You can now play the pads and hear your synth! The selected track's patch is always live for playing.

### Step 3: Arm the Track

Before recording, you need to "arm" the track:

1. Press the **Sample** button (bottom right, labeled with a waveform icon)
2. The Sample button LED turns red
3. The Track 1 LED turns red (indicating it's armed)

### Step 4: Enable Record Mode

1. Press the **Record** button (circular button)
2. The Record button LED turns red - record mode is enabled

### Step 5: Record!

1. Press **Play** to start recording
2. The Play LED turns green - you're recording!
3. Play some notes using the Move pads
4. When you're done, press **Play** again to stop

Track 1 now shows the recording length in the transport display.

### Step 6: Listen Back

Press **Play** to hear your recording. Press **Play** again to stop.

Congratulations - you've made your first recording!

## Adding More Tracks

Now let's add a second instrument:

### Step 7: Select Track 2

Press the **Track 2** row button. The Track 2 LED turns green.

### Step 8: Load a Different Sound

1. **Tap Track 2 again** to open the patch browser
2. Select a different patch (maybe a bass or pad)
3. Press the Jog button to load
4. You return to main view automatically

### Step 9: Record Track 2

1. Press **Sample** to arm Track 2 (LED turns red)
2. Press **Record** to enable record mode (LED turns red)
3. Press **Play** to start
4. Track 1 plays back while you record Track 2!
5. Play your new part along with Track 1
6. Press **Play** to stop

Now you have two tracks playing together!

## Punch-In Recording

Four Track supports punch-in recording in two ways:

### Pause and Continue
1. Record some content on a track
2. Press **Play** to stop (playhead stays where it is)
3. Use **Left arrow** to jump to the start
4. Use **Right arrow** to jump to the end of your recording
5. Arm the track, enable record mode
6. Press **Play** to continue recording from that position

### Live Punch In/Out
1. Start playback with **Play**
2. When you're ready to record, press **Record** to punch in
3. Record your part
4. Press **Record** again to punch out (playback continues)

This is great for fixing a section without stopping the whole transport.

The arrow LEDs turn green when you're at the start or end of the track's content.

## Mixing Your Tracks

After recording, you'll want to balance your tracks:

### Quick Level Adjustment

- **Master Knob**: Adjusts level of the currently selected track
- An overlay shows the current value

### Synth Controls

In the main view, **Knobs 1-8** control your synth's macro parameters (as defined in the Signal Chain patch). Overlays show parameter names and values.

### Full Mixer

1. Press **Menu** to enter Mixer view
2. Visual faders show all 4 track levels
3. Pan ticks at the bottom show stereo position
4. Use **Knobs 1-4** for track levels
5. Use **Knobs 5-8** for panning
6. Press **Menu** or **Back** to return to main view

### Mute and Solo

- **Step 1-4**: Mute tracks 1-4 (press again to unmute)
- **Shift + Step 1-4**: Solo tracks 1-4 (hear only that track)

## Clearing a Track

To clear a track and start over:

1. Select the track
2. Tap the track button to open the patch browser
3. Select "(None)" at the top of the list
4. The patch is cleared

## LED Reference

| LED | State | Meaning |
|-----|-------|---------|
| Track Row | White | Track available |
| Track Row | Green | Track selected |
| Track Row | Red | Track armed for recording |
| Play | White | Stopped |
| Play | Green | Playing/Recording |
| Record | White | Record mode off |
| Record | Red | Record mode on |
| Sample | White | Selected track not armed |
| Sample | Red | Selected track armed |
| Left | White | Normal |
| Left | Green | At start of track content |
| Right | White | Normal |
| Right | Green | At end of track content |

## Keyboard Shortcuts Summary

| Action | Control |
|--------|---------|
| Select track | Track Row buttons or Up/Down |
| Open patch browser | Tap selected track's row button |
| Toggle Mixer view | Menu |
| Go back | Back |
| Play/Stop | Play |
| Toggle record mode | Record |
| Arm/disarm track | Sample |
| Jump to start | Left |
| Jump to end | Right |
| Mute track | Step 1-4 |
| Solo track | Shift + Step 1-4 |
| Synth macros | Knobs 1-8 (main view) |
| Track levels | Knobs 1-4 (mixer view) |
| Track pan | Knobs 5-8 (mixer view) |
| Selected track level | Master knob |

## Troubleshooting

**No sound when playing?**
- Check that tracks aren't muted
- Check track levels (L: value should be > 00)
- Make sure you actually recorded something

**Can't hear synth while selecting?**
- The selected track's patch is always live - check that a patch is loaded
- Check that your Move's volume is up

**Can't record?**
- Make sure a track is armed (Sample button red)
- Make sure record mode is on (Record button red)
- Make sure a patch is loaded on that track

**Patches not showing?**
- Verify Signal Chain is installed
- Check that patches exist in the chain/patches folder

## What's Next?

Now that you know the basics:
- Experiment with different patch combinations
- Try building full arrangements with all 4 tracks
- Use the Mixer view for fine-tuning your sound
- Practice punch-in recording to fix mistakes

Have fun creating!

---

*Four Track - Part of Move Anything*
