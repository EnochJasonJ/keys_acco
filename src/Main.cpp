#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiInput.h"
#include "MappingKeyboard.h"
#include <deque>

namespace
{
constexpr std::array sectionNames { "Main A", "Main B", "Intro", "Fill", "Ending", "Main C", "Break" };

constexpr std::size_t transportActionIndex = 7;

constexpr std::array actions {
    "INTRO", "MAIN_A", "MAIN_B", "MAIN_C", "FILL", "BREAK", "ENDING",
    "START_STOP", "TEMPO_UP", "TEMPO_DOWN"
};

juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "MIDI Arranger";
    options.filenameSuffix = "settings";
    options.folderName = "MIDI Arranger";
    options.osxLibrarySubFolder = "Application Support";
    options.millisecondsBeforeSaving = -1;
    return options;
}

juce::String describe(const MidiEvent& event)
{
    const auto length = std::min(event.size, static_cast<int>(event.bytes.size()));
    const auto hex = juce::String::toHexString(event.bytes.data(), length, 1).toUpperCase();
    const int status = event.bytes[0];
    const auto channel = "  Ch " + juce::String((status & 15) + 1);
    juce::String name;
    switch (status & 0xf0)
    {
        case 0x80: name = "Note Off" + channel; break;
        case 0x90: name = (event.bytes[2] == 0 ? "Note Off" : "Note On") + channel; break;
        case 0xa0: name = "Poly Pressure" + channel; break;
        case 0xb0: name = "Control Change" + channel; break;
        case 0xc0: name = "Program Change" + channel; break;
        case 0xd0: name = "Channel Pressure" + channel; break;
        case 0xe0: name = "Pitch Bend" + channel; break;
        default:
            switch (status)
            {
                case 0xf0: name = "SysEx"; break;
                case 0xf8: name = "Clock"; break;
                case 0xfa: name = "Start"; break;
                case 0xfb: name = "Continue"; break;
                case 0xfc: name = "Stop"; break;
                case 0xfe: name = "Active Sensing"; break;
                default: name = "System"; break;
            }
            break;
    }

    return name + "    " + hex
        + (event.size > length ? " ... (" + juce::String(event.size) + " bytes)" : juce::String());
}

class MainView final : public juce::Component, private juce::Timer, private juce::ListBoxModel
{
public:
    MainView()
    {
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            action.addItem(actions[i], static_cast<int>(i) + 1);
            const auto stored = settings.getIntValue(actions[i], -1);
            if (stored >= 0 && MidiTrigger::valid(stored >> 8, stored & 0xff)
                && (stored >> 8 != MidiTrigger::transportStatus || i == transportActionIndex))
                bindings[i] = MidiTrigger { stored >> 8, stored & 0xff };
        }

        syncPlaybackBindings();
        title.setText("MIDI Arranger", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
        inputLabel.setText("MIDI input", juce::dontSendNotification);
        inputLabel.attachToComponent(&device, false);
        learnLabel.setText("MIDI Learn", juce::dontSendNotification);
        learnLabel.attachToComponent(&action, false);
        mappingTitle.setText("Key mappings", juce::dontSendNotification);
        emptyMappings.setText("Learn an action to see its mapping here.", juce::dontSendNotification);
        monitorLabel.setText("Incoming MIDI", juce::dontSendNotification);
        device.setTitle("MIDI input");
        action.setTitle("Arranger action");
        showTiming.setButtonText("Show clock and sensing");
        monitor.setTitle("Incoming MIDI events");
        monitor.setRowHeight(25);
        monitor.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff252830));
        for (auto* label : { &title, &inputLabel, &inputStatus, &learnLabel, &binding, &activity, &monitorLabel, &playback, &harmony, &mappingTitle, &emptyMappings, &beatLabel })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffe3e8ef));
        action.setSelectedId(1, juce::dontSendNotification);
        for (auto* component : std::array<juce::Component*, 31> {
                 &title, &inputLabel, &device, &inputStatus, &learnLabel, &action, &learn,
                 &remove, &binding, &activity, &monitorLabel, &clear, &monitor,
                 &transport, &introButton, &variationA, &variationB, &fillButton, &endingButton,
                 &playback, &harmony, &mappingTitle, &emptyMappings, &keyboard, &rangeButton,
                 &variationC, &breakButton, &tempoDown, &tempoUp, &beatLabel, &showTiming })
            addAndMakeVisible(component);

        const auto selectMapping = [this](std::size_t index)
        {
            action.setSelectedId(static_cast<int>(index) + 1, juce::dontSendNotification);
            cancelLearn();
            updateBinding();
        };
        keyboard.onSelect = selectMapping;
        for (std::size_t i = 0; i < mappingButtons.size(); ++i)
        {
            auto& button = mappingButtons[i];
            button.setColour(juce::TextButton::buttonColourId, mappingColour(i));
            button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff18212e));
            button.onClick = [selectMapping, i] { selectMapping(i); };
            addChildComponent(button);
        }
        refreshMappingView();

        const auto bindButton = [this](juce::TextButton& button, LiveControls::Command command)
        {
            button.onClick = [this, command] { live.command(command); updatePlayback(); };
        };
        bindButton(transport, LiveControls::startStop);
        bindButton(introButton, LiveControls::intro);
        bindButton(variationA, LiveControls::mainA);
        bindButton(variationB, LiveControls::mainB);
        bindButton(fillButton, LiveControls::fill);
        bindButton(endingButton, LiveControls::ending);
        bindButton(variationC, LiveControls::mainC);
        bindButton(breakButton, LiveControls::breakBar);
        bindButton(tempoDown, LiveControls::tempoDown);
        bindButton(tempoUp, LiveControls::tempoUp);

        rangeButton.onClick = [this]
        {
            if (capturingRange)
                capturingRange = false;
            else
            {
                timerCallback();
                capturingRange = true;
                capturedLow.reset();
            }
            cancelLearn();
            updateRangeView();
            updateBinding();
        };
        updateRangeView();

        device.onChange = [this] { selectDevice(); };
        action.onChange = [this]
        {
            cancelLearn();
            updateBinding();
        };
        learn.onClick = [this]
        {
            if (armed)
                cancelLearn();
            else
            {
                timerCallback();
                live.learning.store(1, std::memory_order_release);
                armed = actionIndex();
            }
            updateBinding();
        };
        remove.onClick = [this]
        {
            bindings[actionIndex()].reset();
            syncPlaybackBindings();
            cancelLearn();
            saveBindings();
            updateBinding();
        };
        clear.onClick = [this]
        {
            rows.clear();
            dropped = 0;
            monitor.updateContent();
            updateMonitorLabel();
        };

        refreshDevices();
        updateBinding();
        deviceChanges = juce::MidiDeviceListConnection::make([this] { refreshDevices(); });
        updatePlayback();
        setSize(920, 745);
    }

    ~MainView() override
    {
        stopTimer();
        deviceChanges = {};
        input.close();
        audio.removeAudioCallback(&arranger);
        audio.closeAudioDevice();
        monitor.setModel(nullptr);
    }

    void paint(juce::Graphics& graphics) override { graphics.fillAll(juce::Colour(0xff202329)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(24);
        title.setBounds(area.removeFromTop(38));
        auto controlsRow = area.removeFromTop(32);
        for (auto* button : { &transport, &introButton, &variationA, &variationB, &variationC, &fillButton, &breakButton, &endingButton })
        {
            button->setBounds(controlsRow.removeFromLeft(76));
            controlsRow.removeFromLeft(8);
        }
        auto statusRow = area.removeFromTop(32).reduced(0, 2);
        tempoUp.setBounds(statusRow.removeFromRight(68));
        statusRow.removeFromRight(6);
        tempoDown.setBounds(statusRow.removeFromRight(68));
        beatLabel.setBounds(statusRow.removeFromRight(86));
        playback.setBounds(statusRow);
        harmony.setBounds(area.removeFromTop(29));
        area.removeFromTop(23);
        auto inputs = area.removeFromTop(32);
        device.setBounds(inputs.removeFromLeft(280));
        inputStatus.setBounds(inputs.withTrimmedLeft(10));
        area.removeFromTop(35);
        auto controls = area.removeFromTop(32);
        action.setBounds(controls.removeFromLeft(145));
        controls.removeFromLeft(8);
        learn.setBounds(controls.removeFromLeft(112));
        controls.removeFromLeft(8);
        remove.setBounds(controls.removeFromLeft(75));
        binding.setBounds(controls.withTrimmedLeft(10));
        activity.setBounds(area.removeFromTop(30));
        auto mappingHeader = area.removeFromTop(27);
        rangeButton.setBounds(mappingHeader.removeFromRight(110).withTrimmedBottom(3));
        mappingTitle.setBounds(mappingHeader);
        keyboard.setBounds(area.removeFromTop(62));
        area.removeFromTop(6);
        if (emptyMappings.isVisible())
            emptyMappings.setBounds(area.removeFromTop(24));
        else
        {
            juce::Rectangle<int> row;
            int column = 0;
            for (auto& button : mappingButtons)
            {
                if (!button.isVisible())
                    continue;
                if (column == 0)
                    row = area.removeFromTop(27).withTrimmedBottom(4);
                const auto width = row.getWidth() / (2 - column);
                button.setBounds(row.removeFromLeft(width).withTrimmedRight(column == 0 ? 8 : 0));
                column = (column + 1) % 2;
            }
        }
        area.removeFromTop(10);
        auto header = area.removeFromTop(31);
        clear.setBounds(header.removeFromRight(88));
        showTiming.setBounds(header.removeFromRight(175).withTrimmedRight(8));
        monitorLabel.setBounds(header);
        area.removeFromTop(6);
        monitor.setBounds(area);
    }

private:
    void cancelLearn()
    {
        armed.reset();
        live.learning.store(capturingRange ? 1 : 0, std::memory_order_release);
    }

    void syncPlaybackBindings()
    {
        live.setBinding(LiveControls::startStop, bindings[transportActionIndex]);
        live.setBinding(LiveControls::mainA, bindings[1]);
        live.setBinding(LiveControls::mainB, bindings[2]);
        live.setBinding(LiveControls::intro, bindings[0]);
        live.setBinding(LiveControls::fill, bindings[4]);
        live.setBinding(LiveControls::ending, bindings[6]);
        live.setBinding(LiveControls::mainC, bindings[3]);
        live.setBinding(LiveControls::breakBar, bindings[5]);
        live.setBinding(LiveControls::tempoUp, bindings[8]);
        live.setBinding(LiveControls::tempoDown, bindings[9]);
    }

    void updatePlayback()
    {
        if (live.isPlaying() && !audioReady)
        {
            audioError = audio.initialiseWithDefaultDevices(0, 2);
            if (audioError.isEmpty() && audio.getCurrentAudioDevice() != nullptr)
            {
                audio.addAudioCallback(&arranger);
                audioReady = true;
            }
            else
            {
                if (audioError.isEmpty())
                    audioError = "No audio output available";
                live.stop();
            }
        }

        const bool playing = live.isPlaying();
        const auto selected = live.requested.load(std::memory_order_relaxed);
        const auto active = arranger.activeSection();
        const auto queued = live.queuedSection();
        const auto sectionName = [](int section) { return sectionNames[static_cast<std::size_t>(section)]; };
        transport.setButtonText(playing ? "Stop" : "Play");
        fillButton.setEnabled(playing && active != endingSection);
        endingButton.setEnabled(playing && active != endingSection);
        breakButton.setEnabled(playing && active != endingSection);
        const auto beat = arranger.currentBeat();
        beatLabel.setText(playing && beat != 0 ? "Beat " + juce::String(beat) + " / 4" : juce::String(),
                          juce::dontSendNotification);
        juce::String playbackText = juce::String(live.bpm()) + " BPM  |  ";
        if (audioError.isNotEmpty())
            playbackText = audioError;
        else if (!playing)
            playbackText += "Stopped  |  " + juce::String(sectionName(selected));
        else if (active < 0)
            playbackText += "Starting";
        else
        {
            playbackText += juce::String(sectionName(active));
            if (active != endingSection)
            {
                if (queued != 0)
                {
                    playbackText += "  |  Queued " + juce::String(sectionName(queued));
                    if (queued != endingSection)
                        playbackText += " then " + juce::String(sectionName(selected));
                }
                else if (active != selected)
                    playbackText += "  |  Next " + juce::String(sectionName(selected));
            }
        }
        playback.setText(playbackText, juce::dontSendNotification);

        const auto chord = live.chord.load(std::memory_order_relaxed);
        constexpr std::array qualities { " major", " minor", "7", "m7", "maj7", "sus2", "sus4", "dim", "aug" };
        harmony.setText(chord < 0 ? juce::String("Full Keyboard  |  Play a chord")
                                  : "Full Keyboard  |  " + juce::String(midiNoteNames[static_cast<std::size_t>(chord % 12)])
                                      + qualities[static_cast<std::size_t>(chord / 12)],
                        juce::dontSendNotification);

        if (playing || activeId.isNotEmpty())
        {
            if (!isTimerRunning())
                startTimerHz(30);
        }
        else
            stopTimer();
    }

    void refreshDevices()
    {
        devices = juce::MidiInput::getAvailableDevices();
        device.clear(juce::dontSendNotification);
        device.addItem("Off", 1);
        int selection = 1;
        for (int i = 0; i < devices.size(); ++i)
        {
            device.addItem(devices[i].name, i + 2);
            if (devices[i].identifier == activeId)
                selection = i + 2;
        }
        device.setSelectedId(selection, juce::dontSendNotification);
        if (selection == 1 && activeId.isNotEmpty())
        {
            selectDevice();
            inputStatus.setText("Input disconnected", juce::dontSendNotification);
        }
        else if (selection == 1)
            inputStatus.setText(devices.isEmpty() ? "No MIDI inputs found" : "Select a MIDI input",
                                juce::dontSendNotification);
    }

    void updateRangeView()
    {
        rangeButton.setEnabled(activeId.isNotEmpty());
        rangeButton.setButtonText(capturingRange ? "Cancel range" : "Set range");
        juce::String label = "Key mappings";
        if (capturingRange)
            label += capturedLow ? "  |  Play the highest key" : "  |  Play the lowest key";
        else if (activeId.isNotEmpty() && !rangeKnown)
            label += "  |  Full MIDI range";
        else if (rangeKnown)
            label += "  |  " + juce::String(lastKey - firstKey + 1) + " keys  |  "
                     + midiNoteName(firstKey) + " to " + midiNoteName(lastKey);
        mappingTitle.setText(label, juce::dontSendNotification);
    }

    juce::String rangeSetting() const { return "keyboardRange." + activeId; }

    void loadRange(const juce::String& deviceName)
    {
        const auto stored = activeId.isNotEmpty() ? settings.getIntValue(rangeSetting(), -1) : -1;
        const auto model = deviceName.toUpperCase().retainCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        const auto low = stored >> 8;
        const auto high = stored & 0xff;
        const bool valid = stored >= 0 && low < high && high < 128;
        const bool psrI500 = model.contains("PSRI500");
        rangeKnown = valid || psrI500;
        firstKey = valid ? low : psrI500 ? 36 : 0;
        lastKey = valid ? high : psrI500 ? 96 : 127;
        keyboard.setRange(firstKey, lastKey);
        updateRangeView();
    }

    void captureRangeNote(int note)
    {
        if (!capturedLow)
            capturedLow = note;
        else if (note > *capturedLow)
        {
            firstKey = *capturedLow;
            lastKey = note;
            rangeKnown = true;
            capturingRange = false;
            settings.setValue(rangeSetting(), (firstKey << 8) | lastKey);
            if (!settings.saveIfNeeded())
                activity.setText("Could not save keyboard range", juce::dontSendNotification);
            keyboard.setRange(firstKey, lastKey);
            cancelLearn();
            updateBinding();
        }
        updateRangeView();
    }

    void selectDevice()
    {
        keyboard.setListening(false);
        input.close();
        activeId.clear();
        capturingRange = false;
        cancelLearn();
        const auto index = device.getSelectedId() - 2;
        if (juce::isPositiveAndBelow(index, devices.size()))
        {
            if (input.open(devices[index].identifier))
            {
                activeId = devices[index].identifier;
                inputStatus.setText("Listening", juce::dontSendNotification);
                timeOrigin.reset();
            }
            else
            {
                device.setSelectedId(1, juce::dontSendNotification);
                inputStatus.setText("Could not open input", juce::dontSendNotification);
            }
        }
        else
            inputStatus.setText(devices.isEmpty() ? "No MIDI inputs found" : "Input off",
                                juce::dontSendNotification);
        loadRange(activeId.isNotEmpty() ? devices[index].name : juce::String());
        keyboard.setListening(activeId.isNotEmpty());
        updateBinding();
        updatePlayback();
    }

    std::size_t actionIndex() const { return static_cast<std::size_t>(action.getSelectedId() - 1); }

    void updateBinding()
    {
        const auto index = actionIndex();
        const auto& current = bindings[index];
        keyboard.select(index);
        learn.setButtonText(armed ? "Cancel" : "Learn");
        learn.setEnabled(activeId.isNotEmpty() && !capturingRange);
        remove.setEnabled(current.has_value());
        binding.setText(armed ? "Press a button, move a control, or play a note"
                              : current ? current->description() : "Unmapped",
                        juce::dontSendNotification);
    }

    void refreshMappingView()
    {
        bool hasMappings = false;
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& trigger = bindings[i];
            auto& button = mappingButtons[i];
            if (trigger)
            {
                const auto kind = trigger->status & 0xf0;
                juce::String message = trigger->isTransport() ? trigger->description() : kind == 0x90
                    ? midiNoteName(trigger->number) + " (MIDI " + juce::String(trigger->number) + ")"
                    : juce::String(kind == 0xb0 ? "CC " : "Program ") + juce::String(trigger->number);
                button.setButtonText(juce::String(actions[i]) + "  |  " + message
                                     + (trigger->isTransport() ? juce::String()
                                                               : "  |  Ch " + juce::String((trigger->status & 15) + 1)));
                hasMappings = true;
            }
            button.setVisible(trigger.has_value());
        }
        emptyMappings.setVisible(!hasMappings);
        keyboard.repaint();
        resized();
    }

    void saveBindings()
    {
        refreshMappingView();
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            if (const auto& trigger = bindings[i])
                settings.setValue(actions[i], (trigger->status << 8) | trigger->number);
            else
                settings.removeValue(actions[i]);
        }
        if (!settings.saveIfNeeded())
            activity.setText("Could not save MIDI mappings", juce::dontSendNotification);
    }

    void timerCallback() override
    {
        bool changed = false;
        int lastAction = -1;
        bool learned = false;
        const bool captureBatch = capturingRange;
        input.drain([&](const MidiEvent& event)
        {
            if (event.size == 1 && (event.bytes[0] == 0xf8 || event.bytes[0] == 0xfe)
                && !showTiming.getToggleState())
                return;
            if (!timeOrigin)
                timeOrigin = event.time;
            if (rows.size() == 256)
                rows.pop_front();
            rows.push_back(event);
            rows.back().time -= *timeOrigin;
            changed = true;
            if (const auto trigger = MidiTrigger::from(event))
            {
                if (captureBatch)
                {
                    if (capturingRange && (trigger->status & 0xf0) == 0x90)
                        captureRangeNote(trigger->number);
                    return;
                }
                if (armed)
                {
                    if (trigger->isTransport() && *armed != transportActionIndex)
                        return;
                    for (auto& existing : bindings)
                        if (existing == trigger)
                            existing.reset();
                    bindings[*armed] = trigger;
                    armed.reset();
                    learned = true;
                }
                else
                {
                    for (std::size_t i = 0; i < bindings.size(); ++i)
                        if (bindings[i] == trigger)
                            lastAction = static_cast<int>(i);
                }
            }
        });
        if (lastAction >= 0)
            activity.setText("Triggered " + juce::String(actions[static_cast<std::size_t>(lastAction)]),
                             juce::dontSendNotification);
        if (learned)
        {
            syncPlaybackBindings();
            live.learning.store(0, std::memory_order_release);
            updateBinding();
            saveBindings();
        }
        if (changed)
        {
            monitor.updateContent();
            monitor.scrollToEnsureRowIsOnscreen(getNumRows() - 1);
            monitor.repaint();
        }
        if (const auto lost = input.takeDropped(); lost != 0)
        {
            dropped += lost;
            updateMonitorLabel();
        }
        updatePlayback();
    }

    void updateMonitorLabel()
    {
        monitorLabel.setText(dropped == 0 ? juce::String("Incoming MIDI")
            : "Incoming MIDI  |  " + juce::String(dropped) + " dropped", juce::dontSendNotification);
    }

    int getNumRows() override { return static_cast<int>(rows.size()); }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool) override
    {
        if (!juce::isPositiveAndBelow(row, getNumRows()))
            return;
        if (row % 2 == 0)
            graphics.fillAll(juce::Colour(0xff292d35));
        graphics.setColour(juce::Colour(0xffe3e8ef));
        const auto& event = rows[static_cast<std::size_t>(row)];
        const auto text = juce::String(event.time * 1000.0, 1) + " ms    " + describe(event);
        graphics.drawText(text, 10, 0, width - 20, height, juce::Justification::centredLeft, true);
    }

    LiveControls live;
    Arranger arranger { live };
    juce::AudioDeviceManager audio;
    bool audioReady = false;
    juce::String audioError;
    MidiInput input { live };
    juce::PropertiesFile settings { settingsOptions() };
    juce::Array<juce::MidiDeviceInfo> devices;
    juce::String activeId;
    juce::MidiDeviceListConnection deviceChanges;
    std::array<std::optional<MidiTrigger>, actions.size()> bindings {};
    std::optional<std::size_t> armed;
    std::optional<int> capturedLow;
    int firstKey = 0;
    int lastKey = 127;
    bool rangeKnown = false;
    bool capturingRange = false;
    std::optional<double> timeOrigin;
    std::deque<MidiEvent> rows;
    juce::uint64 dropped = 0;

    juce::Label title, inputLabel, inputStatus, learnLabel, binding, activity, monitorLabel, playback, harmony;
    juce::Label mappingTitle, emptyMappings, beatLabel;
    MappingKeyboard<actions.size()> keyboard { bindings, actions, live };
    std::array<juce::TextButton, actions.size()> mappingButtons;
    juce::TooltipWindow keyTooltips { this };
    juce::ComboBox device, action;
    juce::TextButton learn { "Learn" }, remove { "Remove" }, clear { "Clear" }, rangeButton { "Set range" };
    juce::TextButton transport { "Play" }, introButton { "Intro" }, variationA { "Main A" },
                     variationB { "Main B" }, variationC { "Main C" }, fillButton { "Fill" },
                     breakButton { "Break" }, endingButton { "Ending" },
                     tempoDown { "BPM -" }, tempoUp { "BPM +" };
    juce::ToggleButton showTiming;
    juce::ListBox monitor { "Incoming MIDI", this };
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow() : DocumentWindow("MIDI Arranger", juce::Colour(0xff202329), allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainView, true);
        setResizable(true, false);
        setResizeLimits(740, 660, 2000, 1400);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class ArrangerApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MIDI Arranger"; }
    const juce::String getApplicationVersion() override { return "0.4.1"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<MainWindow> window;
};
}

START_JUCE_APPLICATION(ArrangerApplication)
