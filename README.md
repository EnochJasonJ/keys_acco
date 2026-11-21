# MIDI Arranger

A native C++20/JUCE desktop prototype with MIDI Learn and live accompaniment. macOS is the first supported build and test platform.

## Build on macOS

With Apple Command Line Tools and CMake 3.22 or newer, run from this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
open "build/MidiArranger_artefacts/Release/MIDI Arranger.app"
```

CMake verifies the pinned JUCE 8.0.12 archive. An offline build can use an extracted copy through `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE-8.0.12`.

## Play with a keyboard

1. Connect the keyboard and select its port under **MIDI input**. Played notes appear in **Incoming MIDI**.
2. Hold a low C with your left hand, with E and G in your right. The **Full Keyboard** display should show **C major**. Add a passing D while holding the other notes and the display should stay on **C major**.
3. Click **Play** to hear the built-in 108 BPM drums and bass through the Mac's default audio output. Try A, C and E to change to A minor. The bass follows the recognised chord on its next scheduled note.
4. Click **Main B**, then **Fill** before the next downbeat. The next bar is a one-bar fill, followed by Main B. Selecting Main B on its own switches at the next bar without a fill.
5. With playback stopped, click **Intro** to start with a one-bar intro before the selected Main variation. Click **Ending** during playback to play a one-bar ending at the next downbeat and then stop. **Stop** is available during any section.
6. Use **MIDI Learn** for **START_STOP**, **INTRO**, **MAIN_A**, **MAIN_B**, **FILL** and **ENDING**. The other actions still report triggers in the monitor. Mappings save across launches and apply to the selected MIDI input. Select the action and click **Remove** to clear it.

MIDI Learn matches channel and note, controller, or program number. A nonzero control-change value can trigger on every message, so use a button or switch for transport when possible. Learn accepts notes, but those notes also participate in chord detection.

## Playback and MIDI behavior

Full Keyboard recognises major and minor triads, dominant sevenths, minor sevenths, and major sevenths. It matches distinct held pitch classes across the full keyboard, including inversions, octave doublings, and two-hand voicings. Extra notes retain the previous chord unless they complete another supported voicing. For example, adding Bb to C–E–G forms C7. Releasing one note of a seventh does not immediately reduce it to a triad. The detector tracks physical key holds from MIDI Note On and Note Off, so sustain pedal changes have no effect on chord recognition. When there is no recognised chord, the loop uses C as its starting harmony. The prototype generates accompaniment only.

The MIDI callback updates fixed-size note state and publishes the detected chord and mapped transport commands through lock-free atomics. The independent audio callback schedules the loop by sample count. Device setup, monitoring, display, and settings run off the audio thread. The monitor drains its own bounded queue at 30 Hz and stores the latest 256 events. Audio output opens on the first Play request.

Intro, Fill, and Ending each run for one bar. The latest pending one-bar section takes precedence over a plain Main change. A Main selection made during Intro or Fill determines the following variation. Ending finishes playback and clears pending sections.
