#pragma once

#include "Arranger.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

constexpr std::array midiNoteNames { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

inline juce::String midiNoteName(int note)
{
    return juce::String(midiNoteNames[static_cast<std::size_t>(note % 12)]) + juce::String(note / 12 - 1);
}

inline juce::Colour mappingColour(std::size_t index)
{
    constexpr std::array colours {
        0xff83cdc1u, 0xff8ab6f5u, 0xffb4a0f3u, 0xffd9a8f0u, 0xffefb87cu,
        0xffe99ba4u, 0xfff1d474u, 0xffa9ce83u, 0xff9ed2f0u, 0xffc0b5a5u
    };
    return juce::Colour(colours[index % colours.size()]);
}

template <std::size_t Count>
class MappingKeyboard final : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    using Bindings = std::array<std::optional<MidiTrigger>, Count>;
    using Names = std::array<const char*, Count>;

    MappingKeyboard(const Bindings& mappings, const Names& names, const LiveControls& controls)
        : bindings(mappings), actions(names), live(controls)
    {
        setTitle("MIDI mapping keyboard");
        setDescription("Highlighted notes have saved MIDI mappings. Use the action buttons below to inspect them.");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    std::function<void(std::size_t)> onSelect;

    void setListening(bool listening)
    {
        if (listening)
        {
            timerCallback();
            startTimerHz(60);
        }
        else
        {
            stopTimer();
            if (held != LiveControls::HeldNotes {})
            {
                held = {};
                repaint();
            }
        }
    }

    void setRange(int first, int last)
    {
        firstNote = first;
        lastNote = last;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        for (const bool black : { false, true })
            for (int note = firstNote; note <= lastNote; ++note)
                if (isBlack(note) == black)
                    paintKey(g, note, black);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        const auto note = noteAt(event.position);
        if (note < 0 || !onSelect)
            return;

        // Cycle when multiple channels map the same pitch.
        for (std::size_t offset = 1; offset <= Count; ++offset)
        {
            const auto index = (selected + offset) % Count;
            if (isMappedTo(index, note))
            {
                onSelect(index);
                return;
            }
        }
    }

    juce::String getTooltip() override
    {
        const auto note = noteAt(getMouseXYRelative().toFloat());
        if (note < 0)
            return {};
        juce::String result;
        for (std::size_t i = 0; i < Count; ++i)
            if (isMappedTo(i, note))
                result += "\n" + juce::String(actions[i]) + "  (" + bindings[i]->description() + ")";
        return result.isEmpty() ? juce::String() : midiNoteName(note) + result;
    }

    void select(std::size_t index)
    {
        selected = index;
        repaint();
    }

private:
    static bool isBlack(int note)
    {
        return (0x54au & (1u << (note % 12))) != 0;
    }

    static float naturalPosition(int note)
    {
        constexpr std::array whiteBefore { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };
        return static_cast<float>((note / 12) * 7 + whiteBefore[static_cast<std::size_t>(note % 12)]);
    }

    juce::Rectangle<float> keyBounds(int note) const
    {
        const float first = naturalPosition(firstNote) - (isBlack(firstNote) ? 0.31f : 0.0f);
        const float last = naturalPosition(lastNote) + (isBlack(lastNote) ? 0.31f : 1.0f);
        const float width = static_cast<float>(getWidth()) / (last - first);
        const auto x = (naturalPosition(note) - first) * width;
        const auto height = static_cast<float>(getHeight());
        if (isBlack(note))
            return { x - 0.31f * width, 0.0f, 0.62f * width, 0.62f * height };
        return { x, 0.0f, width, height };
    }

    static bool isHeld(const LiveControls::HeldNotes& state, int note)
    {
        return (state[static_cast<std::size_t>(note / 32)] & (std::uint32_t { 1 } << (note % 32))) != 0;
    }

    void timerCallback() override
    {
        const auto current = live.heldNotes();
        if (held == current)
            return;
        for (int note = firstNote; note <= lastNote; ++note)
            if (isHeld(held, note) != isHeld(current, note))
                repaint(keyBounds(note).getSmallestIntegerContainer());
        held = current;
    }

    bool isMappedTo(std::size_t index, int note) const
    {
        const auto& binding = bindings[index];
        return binding && (binding->status & 0xf0) == 0x90 && binding->number == note;
    }

    int noteAt(juce::Point<float> point) const
    {
        for (const bool black : { true, false })
            for (int note = firstNote; note <= lastNote; ++note)
                if (isBlack(note) == black && keyBounds(note).contains(point))
                    return note;
        return -1;
    }

    void paintKey(juce::Graphics& g, int note, bool black) const
    {
        const auto bounds = keyBounds(note).reduced(0.4f, 0.0f);
        const bool pressed = isHeld(held, note);
        g.setColour(juce::Colour(pressed ? 0xff059ab2 : black ? 0xff262c36 : 0xffdce4ed));
        g.fillRect(bounds);

        int matches = 0;
        for (std::size_t i = 0; i < Count; ++i)
            matches += isMappedTo(i, note) ? 1 : 0;
        if (matches > 0)
        {
            auto stripes = bounds.withTrimmedTop(bounds.getHeight() * (black && !pressed ? 0.05f : 0.64f));
            const float stripeHeight = stripes.getHeight() / static_cast<float>(matches);
            for (std::size_t i = 0; i < Count; ++i)
                if (isMappedTo(i, note))
                {
                    auto stripe = stripes.removeFromTop(stripeHeight);
                    g.setColour(mappingColour(i));
                    g.fillRect(stripe);
                    if (i == selected)
                    {
                        g.setColour(juce::Colour(0xff263044));
                        g.drawRect(stripe, 1.0f);
                    }
                }
        }

        if (note % 12 == 0 && matches == 0)
        {
            g.setColour(juce::Colour(0xff3a4555));
            g.setFont(10.0f);
            g.drawText(juce::String(note / 12 - 1),
                       bounds.withTrimmedTop(bounds.getHeight() - 14.0f).toNearestInt(),
                       juce::Justification::centred);
        }
    }

    const Bindings& bindings;
    const Names& actions;
    const LiveControls& live;
    LiveControls::HeldNotes held {};
    int firstNote = 0;
    int lastNote = 127;
    std::size_t selected = 0;
};
