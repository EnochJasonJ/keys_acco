#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiInput.h"
#include "MappingKeyboard.h"
#include "MidiTake.h"
#include "StyleFile.h"
#include "TimelineGrid.h"
#include "PianoRoll.h"
#include <deque>

namespace
{
constexpr std::size_t transportActionIndex = 7;

constexpr std::array actions {
    "INTRO", "MAIN_A", "MAIN_B", "MAIN_C", "FILL", "BREAK", "ENDING",
    "START_STOP", "TEMPO_UP", "TEMPO_DOWN"
};

constexpr std::array commands {
    LiveControls::intro, LiveControls::mainA, LiveControls::mainB, LiveControls::mainC,
    LiveControls::fill, LiveControls::breakBar, LiveControls::ending,
    LiveControls::startStop, LiveControls::tempoUp, LiveControls::tempoDown
};
constexpr std::array defaultShortcuts { 'I', '1', '2', '3', 'F', 'B', 'E', ' ', '=', '-' };

constexpr int normalizeShortcut(int code)
{
    return code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code;
}

constexpr bool isShortcut(int code)
{
    return code >= ' ' && code <= '~';
}

juce::String shortcutSetting(std::size_t index)
{
    return "shortcut." + juce::String(actions[index]);
}

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

class StudioLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    StudioLookAndFeel()
    {
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3b3e));
        setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff685648));
        setColour(juce::TextButton::textColourOffId, juce::Colour(0xffedeae7));
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff303134));
        setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff505154));
        setColour(juce::ComboBox::textColourId, juce::Colour(0xffedeae7));
        setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff777572));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                              bool over, bool down) override
    {
        auto fill = down ? colour.brighter(0.25f) : over ? colour.brighter(0.12f) : colour;
        if (!button.isEnabled())
            fill = fill.withAlpha(0.40f);
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 2.0f);
        if (button.hasKeyboardFocus(true))
        {
            g.setColour(juce::Colour(0xffefb986));
            g.drawRoundedRectangle(bounds, 2.0f, 1.0f);
        }
    }
};

class MainView final : public juce::Component, private juce::Timer, private juce::ListBoxModel
{
public:
    MainView()
    {
        setLookAndFeel(&studioTheme);
        setWantsKeyboardFocus(true);
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            const auto key = normalizeShortcut(settings.getIntValue(shortcutSetting(i), defaultShortcuts[i]));
            if (isShortcut(key) && std::find(shortcuts.begin(), shortcuts.end(), key) == shortcuts.end())
                shortcuts[i] = key;
            action.addItem(actions[i], static_cast<int>(i) + 1);
            const auto stored = settings.getIntValue(actions[i], -1);
            if (stored >= 0 && MidiTrigger::valid(stored >> 8, stored & 0xff)
                && (stored >> 8 != MidiTrigger::transportStatus || i == transportActionIndex))
                bindings[i] = MidiTrigger { stored >> 8, stored & 0xff };
        }

        syncPlaybackBindings();
        title.setText("Keys", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
        inputLabel.setText("MIDI input", juce::dontSendNotification);
        inputLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb6b2aa));
        learnLabel.setText("MIDI Learn", juce::dontSendNotification);
        learnLabel.attachToComponent(&action, false);
        mappingTitle.setText("Key mappings", juce::dontSendNotification);
        emptyMappings.setText("Learn an action to see its mapping here.", juce::dontSendNotification);
        shortcutLabel.setColour(juce::Label::textColourId, juce::Colour(0xffe8e5df));
        configureTimeline();
        monitorLabel.setText("Incoming MIDI", juce::dontSendNotification);
        device.setTitle("MIDI input");
        action.setTitle("Arranger action");
        showTiming.setButtonText("Show clock and sensing");
        monitor.setTitle("Incoming MIDI events");
        monitor.setRowHeight(25);
        monitor.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff27282a));
        for (auto* label : { &title, &inputLabel, &inputStatus, &learnLabel, &binding, &activity, &monitorLabel, &playback, &harmony, &mappingTitle, &emptyMappings, &beatLabel })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffe8e5df));
        action.setSelectedId(1, juce::dontSendNotification);
        for (auto* component : std::array<juce::Component*, 34> {
                 &title, &inputLabel, &device, &inputStatus, &learnLabel, &action, &learn,
                 &remove, &binding, &activity, &monitorLabel, &clear, &monitor,
                 &transport, &introButton, &variationA, &variationB, &fillButton, &endingButton,
                 &playback, &harmony, &mappingTitle, &emptyMappings, &keyboard, &rangeButton,
                 &variationC, &breakButton, &tempoDown, &tempoUp, &beatLabel, &showTiming,
                 &shortcutLabel, &assignShortcut, &removeShortcut })
            addAndMakeVisible(component);

        const auto selectMapping = [this](std::size_t index)
        {
            action.setSelectedId(static_cast<int>(index) + 1, juce::dontSendNotification);
            capturingShortcut = false;
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

        for (std::size_t i = 0; i < playbackButtons.size(); ++i)
        {
            playbackButtons[i]->onClick = [this, i] { performAction(i); };
        }

        for (auto* button : { &tempoDown, &tempoUp })
        {
            button->setTriggeredOnMouseDown(true);
            button->setRepeatSpeed(350, 90);
        }

        assignShortcut.onClick = [this]
        {
            capturingShortcut = !capturingShortcut;
            cancelLearn();
            if (capturingShortcut)
                grabKeyboardFocus();
            updateBinding();
        };
        removeShortcut.onClick = [this]
        {
            capturingShortcut = false;
            shortcuts[actionIndex()] = 0;
            saveShortcuts();
        };

        rangeButton.onClick = [this]
        {
            capturingShortcut = false;
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
            capturingShortcut = false;
            cancelLearn();
            updateBinding();
        };
        learn.onClick = [this]
        {
            capturingShortcut = false;
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

        for (auto* label : { &playback, &beatLabel, &harmony })
        {
            label->setFont(juce::Font(juce::FontOptions(12.0f)));
            label->setColour(juce::Label::textColourId, juce::Colour(0xffe7d4b1));
        }
        beatLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        beatLabel.setJustificationType(juce::Justification::centred);
        tempoDown.setTitle("Decrease tempo");
        tempoUp.setTitle("Increase tempo");
        addAndMakeVisible(arrangeTab);
        addAndMakeVisible(midiTab);
        arrangeTab.onClick = [this] { showWorkspace(true); };
        midiTab.onClick = [this] { showWorkspace(false); };
        showWorkspace(true);

        refreshDevices();
        updateBinding();
        deviceChanges = juce::MidiDeviceListConnection::make([this] { refreshDevices(); });
        updatePlayback();
        setSize(1200, 750);
        updateRoll(true);
    }

    ~MainView() override
    {
        stopTimer();
        deviceChanges = {};
        input.close();
        audio.removeAudioCallback(&arranger);
        audio.closeAudioDevice();
        monitor.setModel(nullptr);
        timelineViewport.onScroll = nullptr;
        pianoViewport.onScroll = nullptr;
        timelineViewport.setViewedComponent(nullptr);
        pianoViewport.setViewedComponent(nullptr);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff292a2c));
        g.setColour(juce::Colour(0xff363638));
        g.fillRect(getLocalBounds().removeFromTop(44));
        g.setColour(juce::Colour(0xff535257));
        g.drawHorizontalLine(43, 0.0f, static_cast<float>(getWidth()));
        g.setColour(juce::Colour(0xff1d2120));
        g.fillRoundedRectangle(transportDisplay.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xff555b54));
        g.drawRoundedRectangle(transportDisplay.toFloat().reduced(0.5f), 3.0f, 1.0f);
        for (auto* label : { &beatLabel, &harmony })
            g.drawVerticalLine(label == &beatLabel ? label->getRight() : label->getX() - 3,
                               static_cast<float>(transportDisplay.getY() + 5),
                               static_cast<float>(transportDisplay.getBottom() - 5));
        if (!arranging)
            return;

        for (const auto& panel : { trackSidebar, inspectorBounds, editorHeader })
        {
            if (!panel.isEmpty())
            {
                g.setColour(juce::Colour(0xff333436));
                g.fillRect(panel);
                g.setColour(juce::Colour(0xff525255));
                g.drawRect(panel);
            }
        }
        g.setFont(11.0f);
        g.setColour(juce::Colour(0xffd3cfca));
        g.drawText("Tracks", trackSidebar.withHeight(timelineGrid.trackTop()).reduced(9, 0),
                   juce::Justification::centredLeft);
        for (int track = 0; track < trackCount; ++track)
        {
            const int y = trackSidebar.getY() + timelineGrid.trackTop() + track * TimelineGrid::laneHeight;
            g.setColour(juce::Colour(0xff555558));
            g.drawHorizontalLine(y, static_cast<float>(trackSidebar.getX()), static_cast<float>(trackSidebar.getRight()));
            const auto colour = juce::Colour(studio::trackColours[static_cast<std::size_t>(track)]);
            g.setColour(colour);
            const int x = trackSidebar.getX() + 9;
            g.fillRect(x - 4, y + 8, 2, 46);
            if (track == drumsTrack)
            {
                for (int i = 0; i < 4; ++i)
                    g.fillRect(x + 3 + (i % 2) * 5, y + 16 + (i / 2) * 5, 3, 3);
            }
            else
            {
                g.fillEllipse(static_cast<float>(x + 4), static_cast<float>(y + 21), 9, 6);
                g.drawVerticalLine(x + 12, static_cast<float>(y + 13), static_cast<float>(y + 25));
            }
            g.setColour(juce::Colour(0xffb7b2ab));
            g.drawText(track == drumsTrack ? "Built-in kit" : "Built-in bass",
                       x + 19, y + 32, trackSidebar.getWidth() - 33, 16, juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 5);
        auto toolbar = area.removeFromTop(38);
        title.setBounds(toolbar.removeFromLeft(58));
        arrangeTab.setBounds(toolbar.removeFromLeft(66).reduced(1, 6));
        midiTab.setBounds(toolbar.removeFromLeft(82).reduced(1, 6));
        toolbar.removeFromLeft(8);
        auto midi = toolbar.removeFromRight(206).reduced(1, 6);
        inputLabel.setBounds(midi.removeFromLeft(65));
        device.setBounds(midi);
        if (arranging)
            recordButton.setBounds(toolbar.removeFromLeft(34).reduced(2, 6));
        transport.setBounds(toolbar.removeFromLeft(36).reduced(2, 6));
        toolbar.removeFromLeft(5);

        auto tempo = toolbar.removeFromRight(64).reduced(0, 6);
        tempoUp.setBounds(tempo.removeFromRight(30));
        tempoDown.setBounds(tempo.removeFromLeft(30));
        toolbar.removeFromRight(5);
        transportDisplay = toolbar.reduced(0, 3);
        auto lcd = transportDisplay.reduced(5, 2);
        beatLabel.setBounds(lcd.removeFromLeft(57));
        harmony.setBounds(lcd.removeFromRight(std::min(180, lcd.getWidth() * 2 / 5)).withTrimmedLeft(6));
        playback.setBounds(lcd.withTrimmedLeft(6));

        area.removeFromTop(6);
        auto sections = area.removeFromTop(30);
        for (auto* button : { &introButton, &variationA, &variationB, &variationC, &fillButton, &breakButton, &endingButton })
            button->setBounds(sections.removeFromLeft(80).reduced(2, 3));
        if (arranging)
            inputStatus.setBounds(sections.removeFromRight(150));
        area.removeFromTop(4);

        if (arranging)
            layoutStudio(area);
        else
            layoutMidi(area);
    }

    void resetPressedShortcuts() noexcept { pressed.fill(ShortcutPress::up); }

    bool keyPressed(const juce::KeyPress& key) override
    {
        for (auto* focused = getCurrentlyFocusedComponent(); focused != nullptr;
             focused = focused->getParentComponent())
            if (dynamic_cast<juce::TextEditor*>(focused) != nullptr)
                return false;

        const auto code = normalizeShortcut(key.getKeyCode());
        if (capturingShortcut && code == juce::KeyPress::escapeKey)
        {
            capturingShortcut = false;
            updateBinding();
            return true;
        }
        if (!isShortcut(code) || key.getModifiers().isAnyModifierKeyDown() || armed || capturingRange)
            return false;

        const auto slot = std::find(shortcuts.begin(), shortcuts.end(), code);
        const bool mapped = slot != shortcuts.end();
        auto& down = pressed[static_cast<std::size_t>(code)];
        if (down != ShortcutPress::up)
        {
            if (down == ShortcutPress::repeatingTempo && mapped && !capturingShortcut
                && isTempoCommand(commands[static_cast<std::size_t>(slot - shortcuts.begin())]))
                performAction(static_cast<std::size_t>(slot - shortcuts.begin()));
            return capturingShortcut || mapped;
        }
        down = ShortcutPress::down;

        if (capturingShortcut)
        {
            if (mapped)
                *slot = 0;
            shortcuts[actionIndex()] = code;
            capturingShortcut = false;
            saveShortcuts();
            return true;
        }
        if (!mapped)
            return false;

        const auto index = static_cast<std::size_t>(slot - shortcuts.begin());
        if (playbackButtons[index]->isEnabled())
        {
            if (isTempoCommand(commands[index]))
                down = ShortcutPress::repeatingTempo;
            performAction(index);
        }
        return true;
    }

    bool keyStateChanged(bool isKeyDown) override
    {
        if (!isKeyDown)
            for (std::size_t i = 0; i < pressed.size(); ++i)
                if (pressed[i] != ShortcutPress::up && !juce::KeyPress::isKeyCurrentlyDown(static_cast<int>(i)))
                    pressed[i] = ShortcutPress::up;
        return false;
    }

private:
    void showWorkspace(bool arrangement)
    {
        arranging = arrangement;
        arrangeTab.setToggleState(arranging, juce::dontSendNotification);
        midiTab.setToggleState(!arranging, juce::dontSendNotification);

        for (auto* component : std::array<juce::Component*, 16> {
                 &timelineTitle, &timelineStatus, &timelineViewport, &openStyleButton, &saveStyleButton,
                 &firstBar, &lastBar, &targetSection, &assignBars, &recordButton, &recordStatus, &addBarButton,
                 &trackHeaders[0], &trackHeaders[1], &drumKeyGuide, &selectionLabel })
            component->setVisible(arranging);
        drumKeyGuide.setVisible(arranging && recordTrack.getSelectedId() == drumsTrack + 1);
        for (auto* component : std::array<juce::Component*, 6> {
                 &inspectorButton, &editorButton, &zoomOut, &zoomIn, &trackArm[0], &trackArm[1] })
            component->setVisible(arranging);
        for (auto* component : std::array<juce::Component*, 3> { &inspectorTrack, &inspectorInstrument, &sectionLabel })
            component->setVisible(arranging && inspectorOpen);
        for (auto* component : std::array<juce::Component*, 4> { &pianoTitle, &pianoViewport, &pianoKeys, &pianoRuler })
            component->setVisible(arranging && editorOpen);

        for (auto* component : std::array<juce::Component*, 17> {
                 &learnLabel, &action, &learn, &remove, &binding, &assignShortcut, &removeShortcut,
                 &shortcutLabel, &activity, &mappingTitle, &rangeButton, &keyboard, &monitorLabel, &monitor,
                 &showTiming, &clear, &midiSetupTitle })
            component->setVisible(!arranging);
        for (std::size_t i = 0; i < mappingButtons.size(); ++i)
            mappingButtons[i].setVisible(!arranging && bindings[i].has_value());
        emptyMappings.setVisible(!arranging && std::none_of(bindings.begin(), bindings.end(),
                                                            [](const auto& item) { return item.has_value(); }));
        resized();
        repaint();
    }

    void layoutStudio(juce::Rectangle<int> area)
    {
        auto top = area.removeFromTop(28);
        inspectorButton.setToggleState(inspectorOpen, juce::dontSendNotification);
        editorButton.setToggleState(editorOpen, juce::dontSendNotification);
        inspectorButton.setBounds(top.removeFromLeft(78).reduced(1, 2));
        editorButton.setBounds(top.removeFromLeft(88).reduced(1, 2));
        zoomIn.setBounds(top.removeFromRight(31).reduced(1, 2));
        zoomOut.setBounds(top.removeFromRight(31).reduced(1, 2));
        addBarButton.setBounds(top.removeFromRight(61).reduced(1, 2));
        saveStyleButton.setBounds(top.removeFromRight(78).reduced(1, 2));
        openStyleButton.setBounds(top.removeFromRight(78).reduced(1, 2));
        timelineTitle.setBounds(top.withTrimmedLeft(6));
        area.removeFromTop(3);

        auto footer = area.removeFromBottom(26);
        recordStatus.setBounds(footer.removeFromLeft(192));
        drumKeyGuide.setBounds(footer);
        if (!inspectorOpen)
        {
            auto strip = area.removeFromBottom(30);
            selectionLabel.setBounds(strip.removeFromLeft(70));
            firstBar.setBounds(strip.removeFromLeft(76).reduced(1, 3));
            lastBar.setBounds(strip.removeFromLeft(76).reduced(1, 3));
            targetSection.setBounds(strip.removeFromLeft(92).reduced(1, 3));
            assignBars.setBounds(strip.removeFromLeft(85).reduced(1, 3));
            timelineStatus.setBounds(strip.withTrimmedLeft(4));
            inspectorBounds = {};
        }
        else
        {
            inspectorBounds = area.removeFromLeft(180);
            area.removeFromLeft(4);
            auto inside = inspectorBounds.reduced(9);
            inspectorTrack.setBounds(inside.removeFromTop(27));
            inspectorInstrument.setBounds(inside.removeFromTop(20));
            inside.removeFromTop(15);
            selectionLabel.setBounds(inside.removeFromTop(21));
            auto bounds = inside.removeFromTop(27);
            firstBar.setBounds(bounds.removeFromLeft(bounds.getWidth() / 2).reduced(1));
            lastBar.setBounds(bounds.reduced(1));
            inside.removeFromTop(9);
            sectionLabel.setBounds(inside.removeFromTop(21));
            targetSection.setBounds(inside.removeFromTop(26).reduced(1));
            timelineStatus.setBounds(inside.removeFromTop(23));
            assignBars.setBounds(inside.removeFromTop(27).reduced(1));
        }

        if (editorOpen)
        {
            auto editor = area.removeFromBottom(std::clamp(area.getHeight() * 2 / 5, 134, 280));
            area.removeFromBottom(4);
            editorHeader = editor.removeFromTop(23);
            auto label = editorHeader;
            pianoTitle.setBounds(label);
            auto ruler = editor.removeFromTop(19);
            ruler.removeFromLeft(trackHeaderWidth);
            pianoRuler.setBounds(ruler.withTrimmedRight(scrollbarSize));
            auto keys = editor.removeFromLeft(trackHeaderWidth);
            pianoKeys.setBounds(keys.withTrimmedBottom(scrollbarSize));
            pianoViewport.setBounds(editor);
        }
        else
            editorHeader = {};

        trackSidebar = area.removeFromLeft(trackHeaderWidth);
        timelineViewport.setBounds(area.withTrimmedRight(scrollbarSize));
        resizeTimelineGrid();
        for (int track = 0; track < trackCount; ++track)
        {
            const int y = trackSidebar.getY() + timelineGrid.trackTop() + track * TimelineGrid::laneHeight;
            const auto idx = static_cast<std::size_t>(track);
            trackHeaders[idx].setBounds(trackSidebar.getX() + 23, y + 9, trackHeaderWidth - 53, 22);
            trackArm[idx].setBounds(trackSidebar.getRight() - 27, y + 9, 20, 22);
        }
    }

    void layoutMidi(juce::Rectangle<int> area)
    {
        auto heading = area.removeFromTop(38);
        inputStatus.setBounds(heading.removeFromRight(220));
        midiSetupTitle.setBounds(heading);
        area.removeFromTop(19);
        auto controls = area.removeFromTop(32);
        action.setBounds(controls.removeFromLeft(145));
        controls.removeFromLeft(8);
        learn.setBounds(controls.removeFromLeft(110));
        controls.removeFromLeft(8);
        remove.setBounds(controls.removeFromLeft(74));
        binding.setBounds(controls.withTrimmedLeft(9));
        area.removeFromTop(6);
        auto shortcutRow = area.removeFromTop(28);
        assignShortcut.setBounds(shortcutRow.removeFromLeft(145));
        shortcutRow.removeFromLeft(8);
        removeShortcut.setBounds(shortcutRow.removeFromLeft(112));
        shortcutLabel.setBounds(shortcutRow.withTrimmedLeft(8));
        activity.setBounds(area.removeFromTop(24));

        auto mappingHeader = area.removeFromTop(28);
        rangeButton.setBounds(mappingHeader.removeFromRight(110).withTrimmedBottom(3));
        mappingTitle.setBounds(mappingHeader);
        keyboard.setBounds(area.removeFromTop(64));
        area.removeFromTop(5);
        if (emptyMappings.isVisible())
            emptyMappings.setBounds(area.removeFromTop(23));
        else
        {
            juce::Rectangle<int> row;
            int column = 0;
            for (auto& button : mappingButtons)
            {
                if (!button.isVisible())
                    continue;
                if (column == 0)
                    row = area.removeFromTop(25).withTrimmedBottom(4);
                const auto width = row.getWidth() / (2 - column);
                button.setBounds(row.removeFromLeft(width).withTrimmedRight(column == 0 ? 7 : 0));
                column = (column + 1) % 2;
            }
        }
        area.removeFromTop(6);
        auto monitorHeader = area.removeFromTop(29);
        clear.setBounds(monitorHeader.removeFromRight(76));
        showTiming.setBounds(monitorHeader.removeFromRight(184).withTrimmedRight(8));
        monitorLabel.setBounds(monitorHeader);
        area.removeFromTop(5);
        monitor.setBounds(area);
    }

    void updateTrackHeaders()
    {
        const auto selected = recordTrack.getSelectedId() - 1;
        for (int track = 0; track < trackCount; ++track)
        {
            const auto idx = static_cast<std::size_t>(track);
            trackHeaders[idx].setToggleState(track == selected, juce::dontSendNotification);
            trackArm[idx].setToggleState(track == selected, juce::dontSendNotification);
        }
        inspectorTrack.setText(selected == drumsTrack ? "Drums" : "Bass", juce::dontSendNotification);
        inspectorInstrument.setText(selected == drumsTrack ? "Built-in kit" : "Built-in monophonic bass",
                                    juce::dontSendNotification);
        drumKeyGuide.setVisible(arranging && selected == drumsTrack);
        updateRoll(true);
    }

    void updateRoll(bool centre)
    {
        const int track = recordTrack.getSelectedId() == bassTrack + 1 ? bassTrack : drumsTrack;
        pianoRoll.showTrack(track, barSelection);
        const int start = barSelection.first * StyleTimeline::ticksPerBar;
        const int end = (barSelection.last + 1) * StyleTimeline::ticksPerBar;
        const bool hasNotes = std::any_of(style.notes.begin(), style.notes.end(), [&](const auto& note)
        {
            return note.track == track && note.tick < end && note.tick + note.length > start;
        });
        pianoTitle.setText(juce::String("Piano Roll  |  ") + (track == drumsTrack ? "Drums" : "Bass")
                           + "  |  Bars " + juce::String(barSelection.first + 1) + "-" + juce::String(barSelection.last + 1)
                           + (hasNotes ? "  |  Read-only" : "  |  No recorded notes in range"),
                           juce::dontSendNotification);
        if (centre && editorOpen)
            pianoViewport.setViewPosition(timelineViewport.getViewPositionX(),
                studio::Scale::pitchY(pianoRoll.centrePitch()) - pianoViewport.getHeight() / 2);
    }

    void configureEditor()
    {
        for (auto* label : { &inspectorTrack, &inspectorInstrument, &sectionLabel, &pianoTitle })
        {
            label->setColour(juce::Label::textColourId, juce::Colour(0xffd9d3cb));
            addAndMakeVisible(label);
        }
        inspectorTrack.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        sectionLabel.setText("Arranger section", juce::dontSendNotification);
        selectionLabel.setText("Bar range", juce::dontSendNotification);
        for (auto* component : std::array<juce::Component*, 9> {
                 &inspectorButton, &editorButton, &zoomIn, &zoomOut, &pianoViewport, &pianoKeys, &pianoRuler,
                 &trackArm[0], &trackArm[1] })
            addAndMakeVisible(component);
        pianoViewport.setViewedComponent(&pianoRoll, false);
        pianoViewport.setScrollBarsShown(true, true);
        pianoViewport.setScrollBarThickness(scrollbarSize);
        timelineViewport.setScrollBarThickness(scrollbarSize);

        timelineViewport.onScroll = [this](juce::Rectangle<int> bounds)
        {
            if (editorOpen && !synchronizingScroll)
            {
                const juce::ScopedValueSetter<bool> guard(synchronizingScroll, true);
                pianoViewport.setViewPosition(bounds.getX(), pianoViewport.getViewPositionY());
                pianoRuler.setScale(timelineGrid.getWidth(), bounds.getX());
            }
        };
        pianoViewport.onScroll = [this](juce::Rectangle<int> bounds)
        {
            pianoKeys.setScrollY(bounds.getY());
            pianoRuler.setScale(timelineGrid.getWidth(), bounds.getX());
            if (!synchronizingScroll && editorOpen)
            {
                const juce::ScopedValueSetter<bool> guard(synchronizingScroll, true);
                timelineViewport.setViewPosition(bounds.getX(), 0);
            }
        };
        inspectorButton.onClick = [this] { inspectorOpen = !inspectorOpen; showWorkspace(true); };
        editorButton.onClick = [this]
        {
            editorOpen = !editorOpen;
            showWorkspace(true);
            updateRoll(true);
        };
        zoomOut.onClick = [this] { changeZoom(-22); };
        zoomIn.onClick = [this] { changeZoom(22); };
        recordButton.setTitle("Record MIDI");
        transport.setTitle("Play / Stop");
    }

    void changeZoom(int amount)
    {
        const int oldWidth = timelineGrid.getWidth();
        const int oldX = timelineViewport.getViewPositionX();
        zoom = juce::jlimit(66, 260, zoom + amount);
        resizeTimelineGrid();
        timelineViewport.setViewPosition(oldWidth > 0 ? oldX * timelineGrid.getWidth() / oldWidth : 0, 0);
    }

    void configureTimeline()
    {
        for (auto* label : { &timelineTitle, &timelineStatus, &recordStatus, &drumKeyGuide })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffe8e5df));
        for (auto* component : std::array<juce::Component*, 14> {
                 &timelineTitle, &timelineStatus, &timelineViewport, &openStyleButton, &saveStyleButton,
                 &firstBar, &lastBar, &targetSection, &assignBars, &recordTrack, &recordButton, &recordStatus, &addBarButton, &drumKeyGuide })
            addAndMakeVisible(component);

        timelineViewport.setViewedComponent(&timelineGrid, false);
        timelineViewport.setScrollBarsShown(false, true);
        timelineViewport.setScrollBarThickness(12);
        firstBar.setTitle("First bar");
        lastBar.setTitle("Last bar");
        targetSection.setTitle("Section to assign");
        for (int i = 0; i < sectionCount; ++i)
            targetSection.addItem(sectionNames[static_cast<std::size_t>(i)], i + 1);
        targetSection.setSelectedId(mainASection + 1, juce::dontSendNotification);
        targetSection.onChange = [this] { updateTimelineStatus(); };
        timelineGrid.onSelection = [this](BarRange range) { selectBars(range); };
        timelineGrid.onSection = [this](int section)
        {
            targetSection.setSelectedId(section + 1, juce::dontSendNotification);
            selectBars(style.assignments[static_cast<std::size_t>(section)]);
        };
        firstBar.onChange = [this]
        {
            const int first = firstBar.getSelectedId() - 1;
            selectBars({ first, std::max(first, barSelection.last) });
        };
        lastBar.onChange = [this]
        {
            const int last = lastBar.getSelectedId() - 1;
            selectBars({ std::min(last, barSelection.first), last });
        };

        assignBars.onClick = [this]
        {
            const auto index = static_cast<std::size_t>(targetSection.getSelectedId() - 1);
            style.assignments[index] = barSelection;
            arranger.setRange(index, barSelection);
            styleDirty = true;
            timelineGrid.refresh();
            resized();
            updateTimelineStatus();
        };
        openStyleButton.onClick = [this] { chooseStyleFile(false); };
        saveStyleButton.onClick = [this] { chooseStyleFile(true); };
        recordTrack.addItem("Drums", drumsTrack + 1);
        recordTrack.addItem("Bass", bassTrack + 1);
        recordTrack.setSelectedId(drumsTrack + 1, juce::dontSendNotification);
        recordTrack.setTitle("Track to record");
        recordTrack.setVisible(false);
        const auto noteLabel = [](int pitch)
        {
            return midiNoteName(pitch) + " (" + juce::String(pitch) + ")";
        };
        drumKeyGuide.setText("Kick " + noteLabel(kickNotes[0]) + " / " + noteLabel(kickNotes[1])
                             + "   |   Snare " + noteLabel(snareNotes[0]) + " / " + noteLabel(snareNotes[1])
                             + "   |   Hi-hat " + noteLabel(standardHatNote) + " or any other key",
                             juce::dontSendNotification);
        selectionLabel.setText("Bar range", juce::dontSendNotification);
        selectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb6b2aa));
        midiSetupTitle.setText("MIDI Setup", juce::dontSendNotification);
        midiSetupTitle.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
        midiSetupTitle.setColour(juce::Label::textColourId, juce::Colour(0xffedf2fa));
        for (auto* label : { &midiSetupTitle, &selectionLabel })
            addAndMakeVisible(label);
        timelineTitle.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        recordButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff963c55));
        for (int track = 0; track < trackCount; ++track)
        {
            auto& button = trackHeaders[static_cast<std::size_t>(track)];
            button.setButtonText(track == drumsTrack ? "Drums" : "Bass");
            button.setTitle(track == drumsTrack ? "Select drums track" : "Select bass track");
            button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3d3e40));
            button.setColour(juce::TextButton::buttonOnColourId,
                             juce::Colour(studio::trackColours[static_cast<std::size_t>(track)]).overlaidWith(
                                 juce::Colour(0xff363637).withAlpha(0.68f)));
            button.onClick = [this, track]
            {
                recordTrack.setSelectedId(track + 1, juce::dontSendNotification);
                updateTrackHeaders();
            };
            auto& arm = trackArm[static_cast<std::size_t>(track)];
            arm.setButtonText("R");
            arm.setTitle(track == drumsTrack ? "Arm drums" : "Arm bass");
            arm.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffa14e43));
            arm.onClick = button.onClick;
            addAndMakeVisible(button);
        }
        configureEditor();
        updateTrackHeaders();
        recordButton.onClick = [this]
        {
            if (take)
                stopRecording();
            else
                startRecording();
        };
        addBarButton.onClick = [this]
        {
            if (style.bars.size() >= StyleTimeline::maxBars)
                return;
            style.bars.push_back({ 0, 0, 0, 0 });
            replaceEngineStyle();
            styleDirty = true;
            populateBarSelectors();
            const auto last = static_cast<int>(style.bars.size()) - 1;
            selectBars({ last, last });
            timelineViewport.setViewPosition(timelineGrid.getWidth(), 0);
            updatePlayback();
        };
        populateBarSelectors();
    }

    void resizeTimelineGrid()
    {
        const auto width = std::max(static_cast<int>(style.bars.size()) * zoom, timelineViewport.getWidth());
        const bool scrolling = width > timelineViewport.getWidth();
        const auto height = timelineViewport.getHeight() - (scrolling ? scrollbarSize : 0);
        timelineGrid.setSize(width, std::max(height, timelineGrid.trackTop() + trackCount * TimelineGrid::laneHeight));
        pianoRoll.setSize(width, PianoRoll::noteHeight);
        pianoRuler.setScale(width, timelineViewport.getViewPositionX());
    }

    void populateBarSelectors()
    {
        for (auto* selector : { &firstBar, &lastBar })
        {
            selector->clear(juce::dontSendNotification);
            for (int i = 1; i <= static_cast<int>(style.bars.size()); ++i)
                selector->addItem(juce::String(selector == &firstBar ? "From " : "To ") + juce::String(i), i);
        }
        timelineGrid.refresh();
        selectBars(style.assignments[static_cast<std::size_t>(targetSection.getSelectedId() - 1)]);
        resized();
        resizeTimelineGrid();
        timelineViewport.setViewPosition(0, 0);
    }

    void selectBars(BarRange range)
    {
        if (take)
            return;
        barSelection = range;
        firstBar.setSelectedId(range.first + 1, juce::dontSendNotification);
        lastBar.setSelectedId(range.last + 1, juce::dontSendNotification);
        timelineGrid.showSelection(range);
        updateRoll(true);
        updateTimelineStatus();
    }

    void updateTimelineStatus()
    {
        timelineTitle.setText("Style timeline  |  " + styleName + (styleDirty ? " *" : ""), juce::dontSendNotification);
        const auto section = targetSection.getSelectedId() - 1;
        timelineGrid.showAssignedSection(section);
        const auto assigned = style.assignments[static_cast<std::size_t>(section)];
        timelineStatus.setText("Assigned: " + juce::String(assigned.first + 1) + "-"
                                   + juce::String(assigned.last + 1),
                               juce::dontSendNotification);
    }

    void chooseStyleFile(bool saving)
    {
        if (styleChooser != nullptr)
            return;
        styleChooser = std::make_unique<juce::FileChooser>(saving ? "Save style" : "Open style",
                                                           styleFile, "*.keysstyle");
        const auto flags = juce::FileBrowserComponent::canSelectFiles
            | (saving ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                      : juce::FileBrowserComponent::openMode);
        styleChooser->launchAsync(flags, [safe = juce::Component::SafePointer<MainView>(this), saving](const auto& chooser)
        {
            if (safe == nullptr)
                return;
            const auto file = chooser.getResult();
            safe->styleChooser.reset();
            if (file != juce::File())
                safe->useStyleFile(file, saving);
        });
    }

    void useStyleFile(juce::File file, bool saving)
    {
        if (saving)
        {
            if (file.getFileExtension().isEmpty())
                file = file.withFileExtension(".keysstyle");
            if (!file.replaceWithText(juce::JSON::toString(encodeStyle(style))))
            {
                timelineStatus.setText("Could not save style", juce::dontSendNotification);
                return;
            }
        }
        else
        {
            if (live.isPlaying())
            {
                timelineStatus.setText("Stop playback before opening a style", juce::dontSendNotification);
                return;
            }
            juce::var json;
            auto parsed = file.getSize() > 0 && file.getSize() <= 2 * 1024 * 1024
                && juce::JSON::parse(file.loadFileAsString(), json).wasOk() ? decodeStyle(json) : std::nullopt;
            if (!parsed)
            {
                timelineStatus.setText("Invalid style file", juce::dontSendNotification);
                return;
            }
            style = std::move(*parsed);
            replaceEngineStyle();
            populateBarSelectors();
        }
        styleFile = file;
        styleName = file.getFileNameWithoutExtension();
        styleDirty = false;
        updateTimelineStatus();
    }

    void replaceEngineStyle()
    {
        if (audioReady)
            audio.removeAudioCallback(&arranger);
        arranger.replaceTimeline(style);
        if (audioReady)
            audio.addAudioCallback(&arranger);
    }

    void startRecording()
    {
        if (activeId.isEmpty() || live.isPlaying() || take || capturingRange)
            return;
        timerCallback();
        cancelLearn();
        capturingShortcut = false;
        live.learning.store(1, std::memory_order_release);
        if (!ensureAudio())
        {
            live.learning.store(0, std::memory_order_release);
            updatePlayback();
            return;
        }

        const int track = recordTrack.getSelectedId() - 1;
        recordingRange = barSelection;
        take.emplace(recordingRange, track, live.bpm());
        recordOverflow = false;
        audio.removeAudioCallback(&arranger);
        arranger.prepareRecording(recordingRange, track);
        live.recording.store(1, std::memory_order_release);
        audio.addAudioCallback(&arranger);
        updateRangeView();
        recordStatus.setText("Count-in", juce::dontSendNotification);
        updateBinding();
        updatePlayback();
    }

    void stopRecording()
    {
        const double stopped = juce::Time::getMillisecondCounterHiRes();
        timerCallback();
        if (take)
            finishRecording(stopped);
    }

    void finishRecording(double stopped)
    {
        const double epoch = arranger.recordingEpoch();
        const int last = (recordingRange.last + 1) * StyleTimeline::ticksPerBar;
        const int stopTick = arranger.recordingEnded() ? last : take->tickAt(stopped, epoch);
        live.recording.store(0, std::memory_order_release);

        if (!recordOverflow && epoch > 0 && take->apply(style, stopTick))
        {
            replaceEngineStyle();
            styleDirty = true;
            updateTimelineStatus();
            updateRoll(true);
            recordStatus.setText("Take recorded", juce::dontSendNotification);
        }
        else
            recordStatus.setText(recordOverflow ? "MIDI overflow. Take discarded."
                                                 : stopped < epoch || epoch == 0 ? "Count-in cancelled"
                                                                                : "No take saved",
                                 juce::dontSendNotification);

        take.reset();
        updateRangeView();
        cancelLearn();
        updateBinding();
        updatePlayback();
    }

    void cancelLearn()
    {
        armed.reset();
        live.learning.store(capturingRange || take ? 1 : 0, std::memory_order_release);
    }

    void syncPlaybackBindings()
    {
        for (std::size_t i = 0; i < commands.size(); ++i)
            live.setBinding(commands[i], bindings[i]);
    }

    static bool isTempoCommand(LiveControls::Command command)
    {
        return command == LiveControls::tempoUp || command == LiveControls::tempoDown;
    }

    void performAction(std::size_t index)
    {
        if (take)
        {
            if (index == transportActionIndex)
                stopRecording();
            return;
        }
        live.command(commands[index]);
        updatePlayback();
    }

    static juce::String shortcutName(int code)
    {
        return code == ' ' ? juce::String("Space")
                           : juce::String::charToString(static_cast<juce::juce_wchar>(code));
    }

    void saveShortcuts()
    {
        for (std::size_t i = 0; i < shortcuts.size(); ++i)
            settings.setValue(shortcutSetting(i), shortcuts[i]);
        if (!settings.saveIfNeeded())
            activity.setText("Could not save laptop shortcuts", juce::dontSendNotification);
        updateBinding();
        updatePlayback();
    }

    bool ensureAudio()
    {
        if (audioReady)
            return true;

        audioError = audio.initialiseWithDefaultDevices(0, 2);
        if (audioError.isNotEmpty() || audio.getCurrentAudioDevice() == nullptr)
        {
            if (audioError.isEmpty())
                audioError = "No audio output available";
            return false;
        }
        audio.addAudioCallback(&arranger);
        audioReady = true;
        return true;
    }

    void updatePlayback()
    {
        if (live.isPlaying() && !ensureAudio())
            live.stop();

        const bool playing = live.isPlaying();
        const bool recording = take.has_value();
        openStyleButton.setEnabled(!playing && !recording);
        saveStyleButton.setEnabled(!recording);
        addBarButton.setEnabled(!playing && !recording && style.bars.size() < StyleTimeline::maxBars);
        recordTrack.setEnabled(!recording);
        recordButton.setEnabled(recording || (!playing && activeId.isNotEmpty() && !capturingRange));
        recordButton.setButtonText(juce::String::fromUTF8(recording ? "■" : "●"));
        recordButton.setTooltip(recording ? "Stop recording" : "Record MIDI");
        assignBars.setEnabled(!recording);
        firstBar.setEnabled(!recording);
        lastBar.setEnabled(!recording);
        for (auto& button : trackHeaders)
            button.setEnabled(!recording);
        for (auto& button : trackArm)
            button.setEnabled(!recording);
        const int timelineTick = playing || recording ? arranger.timelineTick() : -1;
        timelineGrid.showPlayhead(timelineTick);
        if (editorOpen)
            pianoRoll.showPlayhead(timelineTick);
        const auto selected = live.requested.load(std::memory_order_relaxed);
        const auto active = arranger.activeSection();
        const auto queued = live.queuedSection();
        const auto sectionName = [](int section) { return sectionNames[static_cast<std::size_t>(section)]; };
        constexpr std::array buttonNames { "Intro", "Main A", "Main B", "Main C", "Fill", "Break",
                                           "Ending", "Play", "BPM +", "BPM -" };
        for (std::size_t i = 0; i < playbackButtons.size(); ++i)
        {
            juce::String text = i == transportActionIndex && (playing || recording) ? "Stop" : buttonNames[i];
            if (shortcuts[i] != 0)
                text += " [" + shortcutName(shortcuts[i]) + "]";
            if (i == transportActionIndex)
            {
                playbackButtons[i]->setButtonText(juce::String::fromUTF8(playing || recording ? "■" : "▶"));
                playbackButtons[i]->setTooltip(text);
            }
            else if (i == 8 || i == 9)
            {
                playbackButtons[i]->setButtonText(i == 8 ? "+" : "-");
                playbackButtons[i]->setTooltip(text);
            }
            else
            {
                playbackButtons[i]->setButtonText(text);
                const auto section = i == 0 ? introSection : i == 1 ? mainASection : i == 2 ? mainBSection
                    : i == 3 ? mainCSection : i == 4 ? fillSection : i == 5 ? breakSection : i == 6 ? endingSection : -1;
                const bool effective = playing && section >= 0 && section == active;
                const auto next = queued != 0 ? queued : selected;
                const bool pending = playing && section >= 0 && section == next && section != active;
                playbackButtons[i]->setColour(juce::TextButton::buttonColourId,
                    juce::Colour(effective ? 0xff477063 : pending ? 0xff896a4b : 0xff3a3b3e));
            }
            playbackButtons[i]->setEnabled(!recording || i == transportActionIndex);
        }
        fillButton.setEnabled(!recording && playing && active != endingSection);
        endingButton.setEnabled(!recording && playing && active != endingSection);
        breakButton.setEnabled(!recording && playing && active != endingSection);
        const auto beat = arranger.currentBeat();
        const bool countingIn = recording && arranger.isCountingIn();
        if (recording)
            recordStatus.setText(countingIn && beat != 0 ? "Count-in  " + juce::String(beat) + " / 4"
                               : countingIn ? juce::String("Count-in") : juce::String("Recording with metronome"),
                                 juce::dontSendNotification);
        const int bar = timelineTick >= 0 ? timelineTick / StyleTimeline::ticksPerBar + 1 : 0;
        beatLabel.setText((playing || recording) && beat != 0
                              ? (bar > 0 ? " " + juce::String(bar) + " . " : "Beat ") + juce::String(beat)
                              : juce::String(" 1 . 1"),
                          juce::dontSendNotification);
        juce::String playbackText = juce::String(live.bpm()) + " BPM  |  ";
        if (audioError.isNotEmpty())
            playbackText = audioError;
        else if (recording)
            playbackText += countingIn ? "Count-in" : "Recording MIDI";
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

        if (playing || recording || activeId.isNotEmpty())
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
        rangeButton.setEnabled(activeId.isNotEmpty() && !take);
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
        if (take)
            stopRecording();
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
        assignShortcut.setEnabled(!capturingRange && !take);
        assignShortcut.setButtonText(capturingShortcut ? "Cancel shortcut" : "Set laptop shortcut");
        removeShortcut.setEnabled(!capturingShortcut && shortcuts[index] != 0);
        shortcutLabel.setText(capturingShortcut ? "Press a key for " + juce::String(actions[index])
                                                : shortcuts[index] == 0 ? juce::String("Laptop shortcut unassigned")
                                                                       : "Laptop shortcut  |  " + shortcutName(shortcuts[index]),
                              juce::dontSendNotification);
        learn.setButtonText(armed ? "Cancel" : "Learn");
        learn.setEnabled(activeId.isNotEmpty() && !capturingRange && !take);
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
            button.setVisible(!arranging && trigger.has_value());
        }
        emptyMappings.setVisible(!arranging && !hasMappings);
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
            if (take)
                take->capture(event, arranger.recordingEpoch());
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
            if (take)
                return;
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
            if (take)
                recordOverflow = true;
            updateMonitorLabel();
        }
        if (take && arranger.recordingEnded())
            finishRecording(juce::Time::getMillisecondCounterHiRes());
        else
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
            graphics.fillAll(juce::Colour(0xff323337));
        graphics.setColour(juce::Colour(0xffe8e5df));
        const auto& event = rows[static_cast<std::size_t>(row)];
        const auto text = juce::String(event.time * 1000.0, 1) + " ms    " + describe(event);
        graphics.drawText(text, 10, 0, width - 20, height, juce::Justification::centredLeft, true);
    }

    StudioLookAndFeel studioTheme;
    static constexpr int trackHeaderWidth = 144;
    static constexpr int scrollbarSize = 11;
    bool arranging = true, inspectorOpen = true, editorOpen = true, synchronizingScroll = false;
    int zoom = TimelineGrid::minimumBarWidth;
    juce::Rectangle<int> trackSidebar, inspectorBounds, editorHeader, transportDisplay;
    juce::TextButton arrangeTab { "Arrange" }, midiTab { "MIDI Setup" };
    juce::TextButton inspectorButton { "Inspector" }, editorButton { "Piano Roll" }, zoomOut { "-" }, zoomIn { "+" };
    std::array<juce::TextButton, trackCount> trackHeaders, trackArm;
    juce::Label selectionLabel, midiSetupTitle, inspectorTrack, inspectorInstrument, sectionLabel, pianoTitle;

    StyleTimeline style = StyleTimeline::starter();
    BarRange barSelection { 0, 0 };
    juce::String styleName { "Starter" };
    juce::File styleFile;
    bool styleDirty = false;
    std::optional<MidiTake> take;
    BarRange recordingRange { 0, 0 };
    bool recordOverflow = false;

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
    std::array<int, actions.size()> shortcuts {};
    enum class ShortcutPress { up, down, repeatingTempo };
    std::array<ShortcutPress, 127> pressed {};
    bool capturingShortcut = false;
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
    juce::Label mappingTitle, emptyMappings, beatLabel, shortcutLabel;
    MappingKeyboard<actions.size()> keyboard { bindings, actions, live };
    std::array<juce::TextButton, actions.size()> mappingButtons;
    juce::TooltipWindow keyTooltips { this };
    juce::ComboBox device, action;
    juce::TextButton learn { "Learn" }, remove { "Remove" }, clear { "Clear" }, rangeButton { "Set range" };
    juce::TextButton transport { "Play" }, introButton { "Intro" }, variationA { "Main A" },
                     variationB { "Main B" }, variationC { "Main C" }, fillButton { "Fill" },
                     breakButton { "Break" }, endingButton { "Ending" },
                     tempoDown { "BPM -" }, tempoUp { "BPM +" };
    std::array<juce::TextButton*, actions.size()> playbackButtons {
        &introButton, &variationA, &variationB, &variationC, &fillButton, &breakButton,
        &endingButton, &transport, &tempoUp, &tempoDown
    };
    juce::TextButton assignShortcut { "Set laptop shortcut" }, removeShortcut { "Remove shortcut" };
    juce::Label timelineTitle, timelineStatus, recordStatus, drumKeyGuide;
    TimelineGrid timelineGrid { style };
    studio::SyncedViewport timelineViewport, pianoViewport;
    PianoRoll pianoRoll { style };
    PianoRollRuler pianoRuler { style };
    PianoRollKeys pianoKeys;
    juce::ComboBox firstBar, lastBar, targetSection, recordTrack;
    juce::TextButton openStyleButton { "Open style" }, saveStyleButton { "Save style" },
                     assignBars { "Assign bars" }, recordButton { "Record MIDI" }, addBarButton { "+ Bar" };
    std::unique_ptr<juce::FileChooser> styleChooser;
    juce::ToggleButton showTiming;
    juce::ListBox monitor { "Incoming MIDI", this };
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow() : DocumentWindow("MIDI Arranger", juce::Colour(0xff27282b), allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainView, true);
        setResizable(true, false);
        setResizeLimits(850, 630, 2000, 1400);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        getContentComponent()->grabKeyboardFocus();
    }

    void activeWindowStatusChanged() override
    {
        DocumentWindow::activeWindowStatusChanged();
        if (auto* view = static_cast<MainView*>(getContentComponent()))
            view->resetPressedShortcuts();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class ArrangerApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MIDI Arranger"; }
    const juce::String getApplicationVersion() override { return "0.7.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<MainWindow> window;
};
}

START_JUCE_APPLICATION(ArrangerApplication)
