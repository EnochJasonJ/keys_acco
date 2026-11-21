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
3. Click **Play** to hear the built-in drums and bass through the Mac's default audio output. Try A, C and E to change to A minor. The bass follows the recognised chord on its next scheduled note.
4. Click **Main B**, then **Fill** during playback. Fill starts immediately, even between beats, plays through the rest of the current bar, and hands off to Main B on the next downbeat. Selecting Main B on its own switches at the next bar.
5. With playback stopped, click **Intro** to start with a one-bar intro before the selected Main variation. Click **Ending** during playback to play a one-bar ending at the next downbeat and then stop. **Stop** is available during any section.
6. Use **MIDI Learn** for **START_STOP**, **INTRO**, **MAIN_A**, **MAIN_B**, **MAIN_C**, **FILL**, **BREAK**, **ENDING**, **TEMPO_UP**, and **TEMPO_DOWN**. Mappings save across launches and apply to the selected MIDI input. Select the action and click **Remove** to clear it.

MIDI Learn matches channel and note, controller, or program number. A nonzero control-change value can trigger on every message, so use a button or switch for transport when possible. Learn accepts notes, but those notes also participate in chord detection.

## Playback and MIDI behavior

Full Keyboard recognises major and minor triads, dominant sevenths, minor sevenths, major sevenths, suspended seconds and fourths, diminished triads, and augmented triads. It matches distinct held pitch classes across the full keyboard, including inversions, octave doublings, and two-hand voicings. Extra notes retain the previous chord unless they complete another supported voicing. For example, adding Bb to C–E–G forms C7. Releasing one note of a seventh does not immediately reduce it to a triad. The detector tracks physical key holds from MIDI Note On and Note Off, so sustain pedal changes have no effect on chord recognition. When there is no recognised chord, the loop uses C as its starting harmony. The prototype generates accompaniment only.

The MIDI callback updates fixed-size note state and publishes the detected chord and mapped transport commands through lock-free atomics. The independent audio callback schedules the loop by sample count. Device setup, monitoring, display, and settings run off the audio thread. The monitor drains its own bounded queue at 30 Hz and stores the latest 256 events. Audio output opens on the first Play request.

Intro and Ending each run for one bar. The latest queued section takes precedence over a plain Main change. Fill starts on the next audio callback without resetting the beat. It plays until the next downbeat, where an already queued Intro, Break, or Ending takes precedence over the selected Main variation. Ending finishes playback and clears pending sections.

## Review saved mappings

**Key mappings** uses the PSR-I500's 61-note range (MIDI 36–96) when the input identifies itself as that model. For another keyboard or a generic USB MIDI port, select **Set range**, then play the lowest and highest physical keys in order. The app remembers the exact range for that MIDI port. Until setup, an unidentified port displays the full MIDI range. If you transpose the MIDI output, run **Set range** again to match the transmitted notes. The piano highlights mapped notes and uses the same colors as the mapping buttons underneath. Hover a mapped key for its note and actions or click it to select the action in MIDI Learn. When several MIDI channels share a pitch, the key shows a stripe for each assignment and clicks cycle through the actions. The list shows saved CC and Program Change assignments alongside note mappings. Note names use C4 for MIDI note 60.

Try C–D–G for Csus2, C–F–G for Csus4, C–Eb–Gb for Cdim, and C–E–Ab for Caug. If a suspended or augmented voicing admits multiple roots, the lowest sounding pitch takes precedence when it is a candidate.

Held notes light up in teal while the piano keeps the mapping colors visible. The highlight follows MIDI Note On and Note Off across channels, independent of the incoming-event monitor and sustain pedal.

## Live tempo and Break

**BPM −** and **BPM +** change tempo by 1 BPM (40–240 BPM). Tempo defaults to 108 BPM at launch. Changes preserve the current beat position. The beat display follows the audio thread’s four-beat bar. **Main C** queues a third groove, and **Break** queues a one-bar stop-time hit on beat 1 with beats 2–4 silent before returning to the selected Main variation.

## Map physical buttons

Clear **Incoming MIDI** and press one hardware button without playing any keys. The monitor hides clock and active sensing by default. A button that sends a Note On, nonzero Control Change, or Program Change can be learned like a key. To learn MIDI Start, Continue, or Stop, select **START_STOP**. Learning any one of the three binds the transport pair. Start and Continue request playback, and Stop requests silence. This controls transport without syncing tempo to MIDI clock.

Some arranger panel buttons control only the keyboard's own functions. If the button sends no assignable event, the app has no MIDI event to map. Generated accompaniment notes are not dedicated button messages.

## Laptop shortcuts

With the arranger window focused, press **Space** for Play/Stop, **1/2/3** for Main A/B/C, **I** for Intro, **F** for Fill, **B** for Break, **E** for Ending, and **-** or **=** for tempo. The keys appear on the playback buttons. Hold a tempo shortcut or either on-screen BPM button to keep changing tempo. The laptop uses the system keyboard repeat rate. Other shortcuts fire once per press. Shortcuts are inactive while typing in a field or capturing a MIDI mapping or keyboard range.

Select an action, click **Set laptop shortcut**, then press an unmodified letter, digit, Space, or punctuation key. Press Escape to cancel. Reusing a key transfers it from the previous action. **Remove shortcut** clears the assignment. Laptop and MIDI assignments save independently.
