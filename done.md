# Keys — implemented features

Current build: **v0.7.0**. Keys is a native C++20/JUCE desktop arranger built with CMake. The Apple Silicon macOS build is the tested target.

## Arrange workspace

- A track-based timeline with separate Drums and Bass lanes, clickable track headers, colored MIDI clips, a bar and beat ruler, a moving playhead, and horizontal scrolling for longer styles.
- A seven-bar starter style with built-in drum and bass grooves. **+ Bar** appends a blank bar. Styles support up to 64 bars in 4/4.
- Select a range by dragging across the timeline or using **From** and **To**, then assign it to Intro, Main A, Main B, Main C, Fill, Break, or Ending. The ruler marks the section assignment, and several sections can reuse the same range.
- Save Style and Open Style persist patterns, recorded notes, and section ranges in local `.keysstyle` files. Earlier style files remain readable. Open a style while transport is stopped.

## Live arranger

- Play/Stop, Intro, Main A/B/C, Fill, Break, and Ending are available from the desktop and through assignable controls.
- Main variations loop their assigned bars and switch on a downbeat. Intro and Break play their assigned ranges once and then hand off. The starter Break hits on beat 1 and leaves beats 2–4 silent. Ending plays its assigned range and stops.
- Fill enters immediately at the current beat position. A one-bar Fill plays through the rest of that bar. A longer Fill continues through the other assigned bars before handing off. The audio clock keeps its beat position throughout.
- Live BPM control covers 40–240 BPM in 1 BPM steps and starts at 108 BPM. The on-screen BPM buttons and laptop tempo shortcuts repeat while held. The status display shows the current beat and active or pending section.
- Full Keyboard chord recognition works across the key range, including two-hand voicings, inversions, and octave doublings. It recognizes major and minor triads, dominant, minor, and major sevenths, sus2, sus4, diminished, and augmented triads. The last chord holds through releases and extra notes unless the held notes complete a supported chord.
- Accompaniment uses built-in drum and monophonic bass sounds. Drums keep their pitches. Bass follows the live chord. Recorded bass is authored against C, then transposed to the detected root with the source major third and perfect fifth adjusted to fit the harmony.

## MIDI recording

- Record inside Keys, one track at a time. Select bars, arm **Drums** or **Bass**, and click **Record MIDI**. Hear the other track and a metronome while recording.
- A one-bar count-in displays beats 1–4 and accents the first click. Recording starts on the following downbeat. Notes played during the count-in stay out of the take.
- Recorded MIDI preserves pitch, note timing, length, and velocity at 960 ticks per beat. The timeline displays the take in its track lane.
- Recording stops at the selected range end or when you click **Stop recording**. A successful take replaces the armed track in the bars reached. The other track stays intact, and an empty take leaves both tracks intact.
- The drum key guide shows kick on B1/C2 (MIDI 35/36), snare on D2/E2 (38/40), and hi-hat on F♯2 (42) or any other note.

## MIDI Setup and controls

- Discover MIDI ports and select one active input. The event monitor shows incoming messages with timestamps and hex bytes. Clock and active sensing are hidden by default and can be shown.
- MIDI Learn saves, reassigns, and removes Note On, nonzero Control Change, and Program Change bindings. **START_STOP** can also learn MIDI Start, Continue, and Stop as a transport binding.
- The piano shows held notes and saved note assignments together. Hover a mapped note for details or click it to select the action. The mapping list also shows MIDI channels, CCs, Program Changes, and transport assignments.
- The visualizer recognizes the PSR-I500’s 61-key range when its port identifies itself. For a generic port, **Set range** learns the lowest and highest keys and saves the range for that port.
- Saved laptop shortcuts work with the Keys window focused. Defaults are **Space** for Play/Stop, **1/2/3** for Main A/B/C, **I** for Intro, **F** for Fill, **B** for Break, **E** for Ending, and **−/=** for tempo. Laptop and MIDI mappings save independently.

## Verification

- Live testing on the Yamaha PSR-I500 confirmed MIDI input, chord recognition, live accompaniment, the keyboard visualizer, physical Start/Stop, laptop shortcuts, immediate Fill, and the first style-assignment workflow.
- macOS automated tests covered audio timing, recording and count-in, style save and reopen, and the Arrange/MIDI Setup layout. Virtual CoreMIDI covered MIDI Learn, recording Drums and Bass, and playing the saved take as Main A and Fill.
- The audio callback schedules by sample count. Device setup, UI, and file access run off the audio thread.
