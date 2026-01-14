/*
 * Four Track Recorder UI
 *
 * A 4-track recorder interface with transport controls, track selection,
 * and signal chain integration.
 */

import * as std from 'std';
import * as os from 'os';
import { isCapacitiveTouchMessage, setButtonLED, setLED, clearAllLEDs } from '../../shared/input_filter.mjs';
import { MoveBack, MoveMenu, MovePlay, MoveRec, MoveRecord, MoveShift,
         MoveUp, MoveDown, MoveLeft, MoveRight, MoveMainKnob, MoveMainButton,
         MoveRow1, MoveRow2, MoveRow3, MoveRow4, MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
         MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8, MoveMaster, MoveCapture, MoveMute, MoveCopy,
         White, Black, BrightRed, BrightGreen, OrangeRed, Cyan, LightGrey,
         WhiteLedDim, WhiteLedBright } from '../../shared/constants.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults,
         showOverlay, tickOverlay, drawOverlay, isOverlayActive } from '../../shared/menu_layout.mjs';
import { createTextScroller } from '../../shared/text_scroll.mjs';

/* ============================================================================
 * Constants
 * ============================================================================ */

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

const NUM_TRACKS = 4;

/* MIDI CCs */
const CC_JOG = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_BACK = MoveBack;
const CC_MENU = MoveMenu;
const CC_PLAY = MovePlay;
const CC_REC = MoveRec;
const CC_RECORD = MoveRecord;
const CC_SHIFT = MoveShift;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_CAPTURE = MoveCapture;
const CC_MUTE = MoveMute;
const CC_COPY = MoveCopy;

/* Track row CCs (for track selection) */
const TRACK_ROWS = [MoveRow1, MoveRow2, MoveRow3, MoveRow4];  /* Top to bottom, matches display */

/* Knobs for track controls (when in mixer view) */
const LEVEL_KNOBS = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4];
const PAN_KNOBS = [MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];

/* All knobs for synth macro mapping */
const ALL_KNOBS = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];

/* Track colors */
const TRACK_COLORS = [
    BrightRed,     /* Track 1 */
    OrangeRed,     /* Track 2 */
    BrightGreen,   /* Track 3 */
    Cyan           /* Track 4 */
];

/* ============================================================================
 * State
 * ============================================================================ */

/* View modes */
const VIEW_MAIN = "main";
const VIEW_PATCH = "patch";
const VIEW_MIXER = "mixer";
const VIEW_SETTINGS = "settings";
let viewMode = VIEW_MAIN;

/* Settings */
const JUMP_OPTIONS = [1, 2, 4, 8];  /* Bars */
let jumpBarsIndex = 0;  /* Index into JUMP_OPTIONS, default 1 bar */
let settingsSelectedItem = 0;  /* 0=tempo, 1=jump, 2=metronome */

/* Track state (synced from DSP) */
let tracks = [];
for (let i = 0; i < NUM_TRACKS; i++) {
    tracks.push({
        level: 0.8,
        pan: 0.0,
        muted: false,
        solo: false,
        armed: false,
        monitoring: true,
        length: 0,
        patch: "Empty"
    });
}

let selectedTrack = 0;
let selectedRow = 0;  /* 0-3 = tracks, 4 = settings */
const ROW_SETTINGS = 4;
let transport = "stopped";
let recordEnabled = false;  /* Record mode - when true, play will start recording */
let tempo = 120;
let metronomeEnabled = false;
let loopEnabled = false;
let playheadMs = 0;

/* Patch browser */
let patches = [];
let patchCount = 0;
let selectedPatch = 0;

/* UI state */
let shiftHeld = false;
let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* Track button state for touch-up detection */
let pressedTrack = -1;  /* Track button currently held down */
let levelAdjustedWhileHeld = false;  /* True if level was changed while holding track */

/* Text scroller for selected track's patch name */
const patchNameScroller = createTextScroller();

/* ============================================================================
 * Helpers
 * ============================================================================ */

function getParam(key) {
    return host_module_get_param(key);
}

function setParam(key, val) {
    host_module_set_param(key, String(val));
}

function syncState() {
    /* Sync transport state */
    transport = getParam("transport") || "stopped";
    selectedTrack = parseInt(getParam("selected_track") || "0");
    tempo = parseInt(getParam("tempo") || "120");
    metronomeEnabled = getParam("metronome") === "1";
    loopEnabled = getParam("loop_enabled") === "1";
    playheadMs = parseInt(getParam("playhead") || "0");

    /* Sync track states */
    for (let i = 0; i < NUM_TRACKS; i++) {
        tracks[i].level = parseFloat(getParam(`track_${i}_level`) || "0.8");
        tracks[i].pan = parseFloat(getParam(`track_${i}_pan`) || "0.0");
        tracks[i].muted = getParam(`track_${i}_muted`) === "1";
        tracks[i].solo = getParam(`track_${i}_solo`) === "1";
        tracks[i].armed = getParam(`track_${i}_armed`) === "1";
        tracks[i].monitoring = getParam(`track_${i}_monitoring`) !== "0";  /* Default true */
        tracks[i].length = parseFloat(getParam(`track_${i}_length`) || "0");
        tracks[i].patch = getParam(`track_${i}_patch`) || "Empty";
    }

    /* Sync patches for browser */
    patchCount = parseInt(getParam("patch_count") || "0");
}

function loadPatches() {
    patches = [];
    /* Add "None" as first option (default) */
    patches.push({ index: -1, name: "(None)" });
    for (let i = 0; i < patchCount; i++) {
        const name = getParam(`patch_name_${i}`);
        if (name) {
            patches.push({ index: i, name: name });
        }
    }
    selectedPatch = 0;  /* Default to "None" */
}

/* Query knob mapping info and show overlay */
function showKnobOverlay(knobNum) {
    const name = getParam(`knob_${knobNum}_name`);
    if (!name) return false;  /* Knob not mapped */

    const value = getParam(`knob_${knobNum}_value`);
    const type = getParam(`knob_${knobNum}_type`);
    const min = parseFloat(getParam(`knob_${knobNum}_min`) || "0");
    const max = parseFloat(getParam(`knob_${knobNum}_max`) || "1");

    /* Format display value */
    let displayValue;
    if (type === "int") {
        displayValue = value;
    } else {
        /* For float values, show percentage or raw value depending on range */
        const floatVal = parseFloat(value);
        if (min === 0 && max === 1) {
            displayValue = `${Math.round(floatVal * 100)}%`;
        } else {
            displayValue = floatVal.toFixed(2);
        }
    }

    showOverlay(name, displayValue);
    return true;
}

function formatTime(ms) {
    const secs = Math.floor(ms / 1000);
    const mins = Math.floor(secs / 60);
    const s = secs % 60;
    const tenths = Math.floor((ms % 1000) / 100);
    return `${mins}:${s.toString().padStart(2, '0')}.${tenths}`;
}

/* ============================================================================
 * LED Control
 * ============================================================================ */

function updateLEDs() {
    /* Track row LEDs - green when selected, red when armed, white otherwise */
    for (let i = 0; i < NUM_TRACKS; i++) {
        let color = White;
        if (tracks[i].armed) {
            color = BrightRed;
        } else if (i === selectedTrack) {
            color = BrightGreen;
        }
        setButtonLED(TRACK_ROWS[i], color);
    }

    /* Transport LEDs */
    /* Play LED - white when stopped, green when playing/recording */
    if (transport === "playing" || transport === "recording") {
        setButtonLED(CC_PLAY, BrightGreen);
    } else {
        setButtonLED(CC_PLAY, White);
    }

    /* Record button (CC_REC) - white when off, red when record mode enabled */
    if (recordEnabled || transport === "recording") {
        setButtonLED(CC_REC, BrightRed);
    } else {
        setButtonLED(CC_REC, White);
    }

    /* Sample button (CC_RECORD) - red if selected track is armed, white otherwise */
    if (tracks[selectedTrack].armed) {
        setButtonLED(CC_RECORD, BrightRed);
    } else {
        setButtonLED(CC_RECORD, White);
    }

    /* Navigation buttons */
    setButtonLED(CC_MENU, WhiteLedBright);
    setButtonLED(CC_BACK, WhiteLedBright);

    /* Left/Right arrows - green at start/end of track content, white otherwise */
    const trackLengthMs = tracks[selectedTrack].length * 1000;
    const atStart = playheadMs <= 0;
    const atEnd = trackLengthMs > 0 && playheadMs >= trackLengthMs - 50;  /* 50ms tolerance */
    setButtonLED(CC_LEFT, atStart ? BrightGreen : White);
    setButtonLED(CC_RIGHT, atEnd ? BrightGreen : White);

    /* Capture button - bright when selected track's monitoring is enabled */
    setButtonLED(CC_CAPTURE, tracks[selectedTrack].monitoring ? WhiteLedBright : WhiteLedDim);

    /* Mute button - bright when selected track is muted */
    const selectedTrackMuted = tracks[selectedTrack]?.muted;
    setButtonLED(CC_MUTE, selectedTrackMuted ? WhiteLedBright : WhiteLedDim);

    /* Copy button - metronome toggle (bright when on) */
    setButtonLED(CC_COPY, metronomeEnabled ? WhiteLedBright : WhiteLedDim);
}

/* ============================================================================
 * Drawing
 * ============================================================================ */

function drawMainView() {
    clear_screen();

    /* Header with transport info */
    const transportIcon = transport === "recording" ? "[REC]" :
                         transport === "playing" ? "[>]" : "[-]";
    const metroIcon = metronomeEnabled ? "[*]" : "";  /* Show [*] when metronome is ON */
    drawMenuHeader("Four Track", `${metroIcon}${transportIcon} ${formatTime(playheadMs)}`);

    /* Calculate scroll offset - show 4 rows at a time */
    const trackHeight = 12;
    const startY = 15;
    const visibleRows = 4;

    /* Scroll so selected row is visible */
    let scrollOffset = 0;
    if (selectedRow >= visibleRows) {
        scrollOffset = selectedRow - visibleRows + 1;
    }

    /* Draw visible rows */
    for (let rowIdx = 0; rowIdx < visibleRows; rowIdx++) {
        const actualRow = rowIdx + scrollOffset;
        const y = startY + rowIdx * trackHeight;

        if (actualRow < NUM_TRACKS) {
            /* Track row */
            const track = tracks[actualRow];
            const isSelected = actualRow === selectedRow;

            /* Track indicators: R for armed, M for monitoring, > for selected */
            let prefix = isSelected ? ">" : " ";
            if (track.armed) prefix = "R";
            const monIcon = track.monitoring ? "M" : " ";

            /* Level/Pan info (right side) - fixed width: "L:XX P:XXX" */
            const displayLevel = track.muted ? 0 : Math.min(99, Math.round(track.level * 100));
            const levelStr = displayLevel.toString().padStart(2, '0');
            let panStr;
            const panVal = Math.round(track.pan * 50);
            if (panVal < -2) {
                panStr = `L${(-panVal).toString().padStart(2, '0')}`;
            } else if (panVal > 2) {
                panStr = `R${panVal.toString().padStart(2, '0')}`;
            } else {
                panStr = "C00";
            }
            const lpInfo = `L:${levelStr} P:${panStr}`;
            const lpInfoWidth = lpInfo.length * 6 + 4;

            /* Track name with patch - account for RM prefix */
            const prefixWidth = 30;  /* "RM T1:" */
            const availableWidth = SCREEN_WIDTH - prefixWidth - lpInfoWidth;
            const maxChars = Math.floor(availableWidth / 6);

            let fullName = `T${actualRow + 1}`;
            if (track.patch !== "Empty") {
                fullName += `: ${track.patch}`;
            }

            let displayName;
            if (isSelected && fullName.length > maxChars) {
                patchNameScroller.setSelected(actualRow);
                displayName = patchNameScroller.getScrolledText(fullName, maxChars);
            } else if (fullName.length > maxChars) {
                displayName = fullName.substring(0, maxChars - 2) + "..";
            } else {
                displayName = fullName;
            }

            /* Draw row */
            if (isSelected) {
                fill_rect(0, y, SCREEN_WIDTH, trackHeight, 1);
                print(2, y + 2, `${prefix}${monIcon}${displayName}`, 0);
                print(SCREEN_WIDTH - lpInfo.length * 6 - 2, y + 2, lpInfo, 0);
            } else {
                print(2, y + 2, `${prefix}${monIcon}${displayName}`, 1);
                print(SCREEN_WIDTH - lpInfo.length * 6 - 2, y + 2, lpInfo, 1);
            }
        } else if (actualRow === ROW_SETTINGS) {
            /* Settings row */
            const isSelected = selectedRow === ROW_SETTINGS;
            const jumpBars = JUMP_OPTIONS[jumpBarsIndex];
            const settingsInfo = `${tempo}bpm ${jumpBars}bar`;

            if (isSelected) {
                fill_rect(0, y, SCREEN_WIDTH, trackHeight, 1);
                print(2, y + 2, "> Settings", 0);
                print(SCREEN_WIDTH - settingsInfo.length * 6 - 2, y + 2, settingsInfo, 0);
            } else {
                print(2, y + 2, "  Settings", 1);
                print(SCREEN_WIDTH - settingsInfo.length * 6 - 2, y + 2, settingsInfo, 1);
            }
        }
    }

    /* Draw overlay if active */
    drawOverlay();
}

function drawPatchView() {
    clear_screen();
    drawMenuHeader(`T${selectedTrack + 1} Patch`, "Select");

    if (patches.length === 0) {
        print(2, 30, "No patches found", 1);
        print(2, 42, "Check chain/patches/", 1);
    } else {
        drawMenuList({
            items: patches,
            selectedIndex: selectedPatch,
            topY: menuLayoutDefaults.listTopY,
            getLabel: (item) => item.name,
            getValue: () => ""
        });
    }

    drawOverlay();
}

function drawSettingsView() {
    clear_screen();
    drawMenuHeader("Settings", "");

    const items = [
        { label: "Tempo", value: `${tempo} BPM` },
        { label: "Jump", value: `${JUMP_OPTIONS[jumpBarsIndex]} bar${JUMP_OPTIONS[jumpBarsIndex] > 1 ? 's' : ''}` },
        { label: "Metronome", value: metronomeEnabled ? "On" : "Off" }
    ];

    const itemHeight = 14;
    const startY = 16;

    for (let i = 0; i < items.length; i++) {
        const y = startY + i * itemHeight;
        const isSelected = i === settingsSelectedItem;

        if (isSelected) {
            fill_rect(0, y, SCREEN_WIDTH, itemHeight, 1);
            print(4, y + 3, items[i].label, 0);
            print(SCREEN_WIDTH - items[i].value.length * 6 - 4, y + 3, items[i].value, 0);
        } else {
            print(4, y + 3, items[i].label, 1);
            print(SCREEN_WIDTH - items[i].value.length * 6 - 4, y + 3, items[i].value, 1);
        }
    }

    /* Instructions */
    print(4, 58, "Jog:adjust  Back:done", 1);

    drawOverlay();
}

function drawMixerView() {
    clear_screen();

    /* Header with transport info */
    const transportIcon = transport === "recording" ? "[REC]" :
                         transport === "playing" ? "[>]" : "[-]";
    const metroIcon = metronomeEnabled ? "[*]" : "";
    drawMenuHeader("Mixer", `${metroIcon}${transportIcon} ${formatTime(playheadMs)}`);

    /* 4 channels across 128px = 32px each */
    const channelWidth = 32;
    const faderWidth = 16;
    const faderHeight = 32;  /* Scaled down to fit below header */
    const labelY = 14;       /* Track labels below header */
    const startY = 24;       /* Faders start with 1px gap after labels */
    const panY = 58;         /* Y position for pan tick area */

    for (let i = 0; i < NUM_TRACKS; i++) {
        const channelX = i * channelWidth;
        const faderX = channelX + (channelWidth - faderWidth) / 2;
        const track = tracks[i];
        const isSelected = i === selectedTrack;

        /* Track number with indicators */
        const label = `${i + 1}`;
        print(channelX + 13, labelY, label, 1);

        /* Selection/arm/monitor indicators */
        if (track.armed) {
            print(channelX + 2, labelY, "R", 1);
        } else if (isSelected) {
            print(channelX + 2, labelY, ">", 1);
        }
        if (track.monitoring) {
            print(channelX + 8, labelY, "M", 1);
        }

        /* Fader background */
        fill_rect(faderX, startY, faderWidth, faderHeight, 1);

        /* Fader fill (from bottom) - show 0 when muted */
        const displayLevel = track.muted ? 0 : track.level;
        const fillHeight = Math.floor(displayLevel * (faderHeight - 2));
        if (fillHeight > 0) {
            fill_rect(faderX + 1, startY + faderHeight - 1 - fillHeight, faderWidth - 2, fillHeight, 0);
        }

        /* Pan tick at bottom - pan range is -1 to +1, map to 0-30px within channel */
        const panRange = channelWidth - 2;
        const panX = channelX + 1 + Math.floor((track.pan + 1) / 2 * panRange);
        /* Draw center line */
        fill_rect(channelX + channelWidth / 2, panY, 1, 5, 1);
        /* Draw pan position tick */
        fill_rect(panX, panY + 1, 2, 3, 1);
    }

    /* Draw overlay if active */
    drawOverlay();
}

function draw() {
    switch (viewMode) {
        case VIEW_MAIN:
            drawMainView();
            break;
        case VIEW_PATCH:
            drawPatchView();
            break;
        case VIEW_MIXER:
            drawMixerView();
            break;
        case VIEW_SETTINGS:
            drawSettingsView();
            break;
    }
}

/* ============================================================================
 * MIDI Handling
 * ============================================================================ */

function handleCC(cc, val) {
    /* Shift state */
    if (cc === CC_SHIFT) {
        shiftHeld = val > 63;
        needsRedraw = true;
        return;
    }

    /* Transport controls */
    if (cc === CC_PLAY && val > 63) {
        if (transport === "stopped") {
            /* If record mode enabled and any track is armed, start recording */
            const anyArmed = tracks.some(t => t.armed);
            if (recordEnabled && anyArmed) {
                setParam("transport", "record");
            } else {
                setParam("transport", "play");
            }
        } else {
            setParam("transport", "stop");
            recordEnabled = false;  /* Clear record mode when stopping */
        }
        syncState();
        needsRedraw = true;
        return;
    }

    /* Record button (CC_REC) - toggle record mode */
    if (cc === CC_REC && val > 63) {
        recordEnabled = !recordEnabled;
        needsRedraw = true;
        return;
    }

    if (cc === CC_RECORD && val > 63) {
        /* Toggle arm on selected track */
        setParam("toggle_arm", String(selectedTrack));
        syncState();
        showOverlay(`T${selectedTrack + 1}`, tracks[selectedTrack].armed ? "Disarmed" : "Armed");
        needsRedraw = true;
        return;
    }

    /* Track row buttons - touch down */
    for (let i = 0; i < NUM_TRACKS; i++) {
        if (cc === TRACK_ROWS[i]) {
            if (val > 63) {
                /* Touch down */
                if (shiftHeld) {
                    /* Shift+Track = toggle arm on that track */
                    setParam("toggle_arm", String(i));
                    syncState();
                    showOverlay(`T${i + 1}`, tracks[i].armed ? "Disarmed" : "Armed");
                } else if (i !== selectedTrack) {
                    /* Switch to different track = select it and return to main view */
                    setParam("select_track", String(i));
                    syncState();
                    if (viewMode === VIEW_PATCH) {
                        viewMode = VIEW_MAIN;
                    }
                }
                /* Remember which track button is pressed */
                pressedTrack = i;
                needsRedraw = true;
            } else {
                /* Touch up - open patch browser if releasing on selected track (and didn't adjust level) */
                if (pressedTrack === selectedTrack && i === selectedTrack && !shiftHeld && !levelAdjustedWhileHeld) {
                    if (viewMode === VIEW_PATCH) {
                        viewMode = VIEW_MAIN;
                    } else {
                        loadPatches();
                        viewMode = VIEW_PATCH;
                    }
                    needsRedraw = true;
                }
                pressedTrack = -1;
                levelAdjustedWhileHeld = false;
            }
            return;
        }
    }

    /* Capture button - toggle monitoring on selected track */
    if (cc === CC_CAPTURE && val > 63) {
        setParam("toggle_monitoring", String(selectedTrack));
        syncState();
        showOverlay(`T${selectedTrack + 1} Monitor`, tracks[selectedTrack].monitoring ? "Off" : "On");
        needsRedraw = true;
        return;
    }

    /* Mute button - toggle mute on selected track */
    if (cc === CC_MUTE && val > 63) {
        const wasMuted = tracks[selectedTrack].muted;
        setParam("toggle_mute", String(selectedTrack));
        syncState();
        /* Show level going to 0 or back to actual level */
        const newLevel = wasMuted ? Math.round(tracks[selectedTrack].level * 100) : 0;
        showOverlay(`T${selectedTrack + 1} Level`, `${newLevel}%`);
        needsRedraw = true;
        return;
    }

    /* Copy button - toggle metronome */
    if (cc === CC_COPY && val > 63) {
        setParam("metronome", metronomeEnabled ? "0" : "1");
        syncState();
        showOverlay("Metronome", metronomeEnabled ? "Off" : "On");
        needsRedraw = true;
        return;
    }

    /* Navigation */
    if (cc === CC_BACK && val > 63) {
        if (viewMode === VIEW_SETTINGS) {
            viewMode = VIEW_MAIN;
        } else if (viewMode !== VIEW_MAIN) {
            viewMode = VIEW_MAIN;
        } else {
            host_return_to_menu();
        }
        needsRedraw = true;
        return;
    }

    if (cc === CC_MENU && val > 63) {
        /* Toggle between main and mixer */
        if (viewMode === VIEW_MIXER) {
            viewMode = VIEW_MAIN;
        } else {
            viewMode = VIEW_MIXER;
        }
        needsRedraw = true;
        return;
    }

    /* Up/Down for row selection in main view */
    if (viewMode === VIEW_MAIN) {
        if (cc === CC_UP && val > 63) {
            if (selectedRow > 0) {
                selectedRow--;
                if (selectedRow < NUM_TRACKS) {
                    selectedTrack = selectedRow;
                    setParam("select_track", String(selectedTrack));
                    syncState();
                }
                needsRedraw = true;
            }
            return;
        }
        if (cc === CC_DOWN && val > 63) {
            if (selectedRow < ROW_SETTINGS) {
                selectedRow++;
                if (selectedRow < NUM_TRACKS) {
                    selectedTrack = selectedRow;
                    setParam("select_track", String(selectedTrack));
                    syncState();
                }
                needsRedraw = true;
            }
            return;
        }
    }

    /* Left/Right = jump by bars, Shift+Left/Right = jump to start/end (works in main and mixer views) */
    if ((viewMode === VIEW_MAIN || viewMode === VIEW_MIXER) && cc === CC_LEFT && val > 63) {
        if (shiftHeld) {
            setParam("goto_start", "1");
            syncState();
            showOverlay("Position", "Start");
        } else {
            const jumpBars = JUMP_OPTIONS[jumpBarsIndex];
            setParam("jump_bars", String(-jumpBars));
            syncState();
            showOverlay("Jump", `-${jumpBars} bar${jumpBars > 1 ? 's' : ''}`);
        }
        needsRedraw = true;
        return;
    }
    if ((viewMode === VIEW_MAIN || viewMode === VIEW_MIXER) && cc === CC_RIGHT && val > 63) {
        if (shiftHeld) {
            setParam("goto_end", "1");
            syncState();
            showOverlay("Position", "End");
        } else {
            const jumpBars = JUMP_OPTIONS[jumpBarsIndex];
            setParam("jump_bars", String(jumpBars));
            syncState();
            showOverlay("Jump", `+${jumpBars} bar${jumpBars > 1 ? 's' : ''}`);
        }
        needsRedraw = true;
        return;
    }

    /* Up/Down in settings view */
    if (viewMode === VIEW_SETTINGS) {
        if (cc === CC_UP && val > 63) {
            if (settingsSelectedItem > 0) {
                settingsSelectedItem--;
                needsRedraw = true;
            }
            return;
        }
        if (cc === CC_DOWN && val > 63) {
            if (settingsSelectedItem < 2) {
                settingsSelectedItem++;
                needsRedraw = true;
            }
            return;
        }
    }

    /* Jog wheel */
    if (cc === CC_JOG) {
        const delta = val < 64 ? val : val - 128;

        if (viewMode === VIEW_PATCH && patches.length > 0) {
            selectedPatch = (selectedPatch + delta + patches.length) % patches.length;
            needsRedraw = true;
        } else if (viewMode === VIEW_SETTINGS) {
            /* Jog adjusts selected setting */
            if (settingsSelectedItem === 0) {
                /* Tempo: adjust by 1 BPM */
                const newTempo = Math.max(20, Math.min(300, tempo + delta));
                setParam("tempo", String(newTempo));
                syncState();
                showOverlay("Tempo", `${newTempo} BPM`);
            } else if (settingsSelectedItem === 1) {
                /* Jump increment: cycle through options */
                jumpBarsIndex = (jumpBarsIndex + (delta > 0 ? 1 : -1) + JUMP_OPTIONS.length) % JUMP_OPTIONS.length;
                showOverlay("Jump", `${JUMP_OPTIONS[jumpBarsIndex]} bars`);
            } else if (settingsSelectedItem === 2) {
                /* Metronome: toggle */
                setParam("metronome", metronomeEnabled ? "0" : "1");
                syncState();
                showOverlay("Metronome", metronomeEnabled ? "Off" : "On");
            }
            needsRedraw = true;
        } else if (viewMode === VIEW_MAIN) {
            /* Jog = scroll through rows (tracks + settings) */
            const newRow = selectedRow + (delta > 0 ? 1 : -1);
            if (newRow >= 0 && newRow <= ROW_SETTINGS) {
                selectedRow = newRow;
                if (newRow < NUM_TRACKS) {
                    selectedTrack = newRow;
                    setParam("select_track", String(newRow));
                    syncState();
                }
                needsRedraw = true;
            }
        }
        return;
    }

    /* Jog click */
    if (cc === CC_JOG_CLICK && val > 63) {
        if (viewMode === VIEW_MAIN) {
            if (selectedRow === ROW_SETTINGS) {
                /* Open settings view */
                viewMode = VIEW_SETTINGS;
                settingsSelectedItem = 0;
            } else {
                /* Open patch browser for selected track */
                loadPatches();
                viewMode = VIEW_PATCH;
            }
            needsRedraw = true;
        } else if (viewMode === VIEW_SETTINGS) {
            /* In settings view: jog click does nothing or could confirm */
            return;
        } else if (viewMode === VIEW_PATCH && patches.length > 0) {
            /* In patch view: load the selected patch and return to main */
            const patchIndex = patches[selectedPatch].index;
            if (patchIndex < 0) {
                /* "None" selected - clear the patch */
                setParam("clear_patch", String(selectedTrack));
                showOverlay("Cleared", `Track ${selectedTrack + 1}`);
            } else {
                setParam("load_patch", String(patchIndex));
                showOverlay("Loaded", patches[selectedPatch].name);
            }
            syncState();
            viewMode = VIEW_MAIN;
            needsRedraw = true;
        }
        return;
    }

    /* Knob handling depends on view mode */
    if (viewMode === VIEW_MIXER) {
        /* In mixer view: knobs 1-4 control levels, 5-8 control pans */

        /* Level knobs (1-4) */
        for (let i = 0; i < 4; i++) {
            if (cc === LEVEL_KNOBS[i]) {
                const delta = val < 64 ? val : val - 128;
                const newLevel = Math.max(0, Math.min(1, tracks[i].level + delta * 0.02));
                setParam("track_level", `${i}:${newLevel.toFixed(2)}`);
                syncState();
                showOverlay(`T${i + 1} Level`, `${Math.round(newLevel * 100)}%`);
                needsRedraw = true;
                return;
            }
        }

        /* Pan knobs (5-8) */
        for (let i = 0; i < 4; i++) {
            if (cc === PAN_KNOBS[i]) {
                const delta = val < 64 ? val : val - 128;
                const newPan = Math.max(-1, Math.min(1, tracks[i].pan + delta * 0.05));
                setParam("track_pan", `${i}:${newPan.toFixed(2)}`);
                syncState();
                const panStr = newPan < -0.1 ? `L${Math.round(-newPan * 50)}` :
                              newPan > 0.1 ? `R${Math.round(newPan * 50)}` : "C";
                showOverlay(`T${i + 1} Pan`, panStr);
                needsRedraw = true;
                return;
            }
        }
    } else {
        /* In main/patch views: knobs control synth macros */
        /* The knob CC is forwarded to DSP by the host, we just need to show overlay */
        for (let i = 0; i < ALL_KNOBS.length; i++) {
            if (cc === ALL_KNOBS[i]) {
                const knobNum = i + 1;  /* Knobs are 1-indexed */
                /* Query the knob mapping info and show overlay */
                if (showKnobOverlay(knobNum)) {
                    needsRedraw = true;
                }
                return;
            }
        }
    }

    /* Master knob for selected track level */
    if (cc === MoveMaster) {
        const delta = val < 64 ? val : val - 128;
        const newLevel = Math.max(0, Math.min(1, tracks[selectedTrack].level + delta * 0.02));
        setParam("track_level", `${selectedTrack}:${newLevel.toFixed(2)}`);
        syncState();
        showOverlay(`T${selectedTrack + 1} Level`, `${Math.round(newLevel * 100)}%`);
        /* Mark that level was adjusted while holding track button */
        if (pressedTrack >= 0) {
            levelAdjustedWhileHeld = true;
        }
        needsRedraw = true;
        return;
    }
}

function handleNote(note, vel) {
    /* Step buttons could be used for mute/solo */
    if (note >= 16 && note <= 19 && vel > 0) {
        const trackIdx = note - 16;
        if (shiftHeld) {
            /* Shift+Step = solo */
            setParam("track_solo", String(trackIdx));
        } else {
            /* Step = mute */
            setParam("track_mute", String(trackIdx));
        }
        syncState();
        needsRedraw = true;
        return;
    }
}

function onMidiMessage(msg, source) {
    if (!msg || msg.length < 3) return;

    /* Filter capacitive touch */
    if (isCapacitiveTouchMessage(msg)) return;

    const status = msg[0] & 0xF0;
    const data1 = msg[1];
    const data2 = msg[2];

    if (status === 0xB0) {
        handleCC(data1, data2);
    } else if (status === 0x90 || status === 0x80) {
        const vel = status === 0x80 ? 0 : data2;
        handleNote(data1, vel);
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

function init() {
    /* Clear LEDs first */
    clearAllLEDs();

    /* Initial sync */
    syncState();
    loadPatches();

    /* Initial LED state */
    updateLEDs();

    /* Initial draw */
    draw();
}

function tick() {
    tickCount++;

    /* Handle overlay timeout */
    if (tickOverlay()) {
        needsRedraw = true;
    }

    /* Tick the patch name scroller */
    if (patchNameScroller.tick()) {
        needsRedraw = true;
    }

    /* Periodic state sync and redraw */
    if (tickCount % REDRAW_INTERVAL === 0) {
        syncState();
        updateLEDs();
        needsRedraw = true;
    }

    if (needsRedraw) {
        draw();
        needsRedraw = false;
    }
}

/* Export module interface */
globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiMessage;
globalThis.onMidiMessageExternal = onMidiMessage;
