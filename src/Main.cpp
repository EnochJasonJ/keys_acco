#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiInput.h"
#include <deque>

namespace
{
constexpr std::array sectionNames { "Main A", "Main B", "Intro", "Fill", "Ending" };

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
            if (stored >= 0 && MidiTrigger::valid(stored >> 8, stored & 0xff))
                bindings[i] = MidiTrigger { stored >> 8, stored & 0xff };
        }

        syncPlaybackBindings();
        playback.setText("108 BPM  |  Full Keyboard", juce::dontSendNotification);
        title.setText("MIDI Arranger", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
        inputLabel.setText("MIDI input", juce::dontSendNotification);
        inputLabel.attachToComponent(&device, false);
        learnLabel.setText("MIDI Learn", juce::dontSendNotification);
        learnLabel.attachToComponent(&action, false);
        monitorLabel.setText("Incoming MIDI", juce::dontSendNotification);
        device.setTitle("MIDI input");
        action.setTitle("Arranger action");
        monitor.setTitle("Incoming MIDI events");
        monitor.setRowHeight(25);
        monitor.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff252830));
        for (auto* label : { &title, &inputLabel, &inputStatus, &learnLabel, &binding, &activity, &monitorLabel, &playback, &harmony })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffe3e8ef));
        action.setSelectedId(1, juce::dontSendNotification);
        for (auto* component : std::array<juce::Component*, 21> {
                 &title, &inputLabel, &device, &inputStatus, &learnLabel, &action, &learn,
                 &remove, &binding, &activity, &monitorLabel, &clear, &monitor,
                 &transport, &introButton, &variationA, &variationB, &fillButton, &endingButton,
                 &playback, &harmony })
            addAndMakeVisible(component);

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
        setSize(780, 600);
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
        for (auto* button : { &transport, &introButton, &variationA, &variationB, &fillButton, &endingButton })
        {
            button->setBounds(controlsRow.removeFromLeft(82));
            controlsRow.removeFromLeft(8);
        }
        playback.setBounds(area.removeFromTop(29));
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
        area.removeFromTop(12);
        auto header = area.removeFromTop(31);
        clear.setBounds(header.removeFromRight(88));
        monitorLabel.setBounds(header);
        area.removeFromTop(6);
        monitor.setBounds(area);
    }

private:
    void cancelLearn()
    {
        armed.reset();
        live.learning.store(0, std::memory_order_release);
    }

    void syncPlaybackBindings()
    {
        live.setBinding(LiveControls::startStop, bindings[7]);
        live.setBinding(LiveControls::mainA, bindings[1]);
        live.setBinding(LiveControls::mainB, bindings[2]);
        live.setBinding(LiveControls::intro, bindings[0]);
        live.setBinding(LiveControls::fill, bindings[4]);
        live.setBinding(LiveControls::ending, bindings[6]);
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
        juce::String playbackText = "108 BPM  |  ";
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

        constexpr std::array names { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        const auto chord = live.chord.load(std::memory_order_relaxed);
        constexpr std::array qualities { " major", " minor", "7", "m7", "maj7" };
        harmony.setText(chord < 0 ? juce::String("Full Keyboard  |  Play a triad or seventh chord")
                                  : "Full Keyboard  |  " + juce::String(names[static_cast<std::size_t>(chord % 12)])
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

    void selectDevice()
    {
        input.close();
        activeId.clear();
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
        updateBinding();
        updatePlayback();
    }

    std::size_t actionIndex() const { return static_cast<std::size_t>(action.getSelectedId() - 1); }

    void updateBinding()
    {
        const auto& current = bindings[actionIndex()];
        learn.setButtonText(armed ? "Cancel" : "Learn");
        learn.setEnabled(activeId.isNotEmpty());
        remove.setEnabled(current.has_value());
        binding.setText(armed ? "Move a control or play a note"
                              : current ? current->description() : "Unmapped",
                        juce::dontSendNotification);
    }

    void saveBindings()
    {
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
        input.drain([&](const MidiEvent& event)
        {
            if (!timeOrigin)
                timeOrigin = event.time;
            if (rows.size() == 256)
                rows.pop_front();
            rows.push_back(event);
            rows.back().time -= *timeOrigin;
            changed = true;
            if (const auto trigger = MidiTrigger::from(event))
            {
                if (armed)
                {
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
    std::optional<double> timeOrigin;
    std::deque<MidiEvent> rows;
    juce::uint64 dropped = 0;

    juce::Label title, inputLabel, inputStatus, learnLabel, binding, activity, monitorLabel, playback, harmony;
    juce::ComboBox device, action;
    juce::TextButton learn { "Learn" }, remove { "Remove" }, clear { "Clear" };
    juce::TextButton transport { "Play" }, introButton { "Intro" }, variationA { "Main A" },
                     variationB { "Main B" }, fillButton { "Fill" }, endingButton { "Ending" };
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
        setResizeLimits(700, 490, 2000, 1400);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class ArrangerApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MIDI Arranger"; }
    const juce::String getApplicationVersion() override { return "0.3.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<MainWindow> window;
};
}

START_JUCE_APPLICATION(ArrangerApplication)
