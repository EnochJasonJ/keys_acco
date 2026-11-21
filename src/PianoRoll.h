#pragma once

#include "TimelineGrid.h"

class PianoRoll final : public juce::Component
{
public:
    explicit PianoRoll(const StyleTimeline& timeline) : style(timeline)
    {
        setTitle("Recorded MIDI piano roll");
        setDescription("Read-only pitch and timing of recorded MIDI notes.");
    }

    static constexpr int noteHeight = 128 * studio::pitchHeight;

    void showTrack(int track, BarRange bars)
    {
        selectedTrack = track;
        selection = bars;
        repaint();
    }

    int centrePitch() const
    {
        const int start = selection.first * StyleTimeline::ticksPerBar;
        const int end = (selection.last + 1) * StyleTimeline::ticksPerBar;
        int low = 128;
        int high = -1;
        for (const auto& note : style.notes)
            if (note.track == selectedTrack && note.tick < end && note.tick + note.length > start)
            {
                low = std::min(low, note.pitch);
                high = std::max(high, note.pitch);
            }
        return high < 0 ? (selectedTrack == drumsTrack ? 42 : 48) : (low + high) / 2;
    }

    void showPlayhead(int tick)
    {
        if (playhead == tick)
            return;
        const auto redraw = [this](int value)
        {
            if (value >= 0)
                repaint(static_cast<int>(scale().tickX(value)) - 1, 0, 3, getHeight());
        };
        redraw(playhead);
        playhead = tick;
        redraw(playhead);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff292a2d));
        const auto clip = g.getClipBounds();
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            const auto y = studio::Scale::pitchY(pitch);
            if (y + studio::pitchHeight < clip.getY() || y >= clip.getBottom())
                continue;
            if (studio::Scale::blackKey(pitch))
            {
                g.setColour(juce::Colour(0xff212225));
                g.fillRect(0, y, getWidth(), studio::pitchHeight);
            }
            g.setColour(juce::Colour(0xff414144));
            g.drawHorizontalLine(y, 0, static_cast<float>(getWidth()));
        }
        const auto mapping = scale();
        const auto start = mapping.tickX(selection.first * StyleTimeline::ticksPerBar);
        const auto end = mapping.tickX((selection.last + 1) * StyleTimeline::ticksPerBar);
        g.setColour(juce::Colour(0xff8fac9e).withAlpha(0.08f));
        g.fillRect(start, 0.0f, end - start, static_cast<float>(getHeight()));
        studio::paintTimeGrid(g, mapping, 0, getHeight(), false);
        for (const auto& note : style.notes)
            if (note.track == selectedTrack)
            {
                const int y = studio::Scale::pitchY(note.pitch) + 2;
                const auto x = mapping.tickX(note.tick);
                const auto width = std::max(1.0f, mapping.tickX(note.length));
                if (y < clip.getY() - studio::pitchHeight || y >= clip.getBottom()
                    || x + width < clip.getX() || x >= clip.getRight())
                    continue;
                g.setColour(juce::Colour(studio::trackColours[static_cast<std::size_t>(selectedTrack)])
                                .withAlpha(0.57f + 0.43f * note.velocity / 127.0f));
                g.fillRect(x, static_cast<float>(y), width, studio::pitchHeight - 4.0f);
            }

        if (playhead >= 0)
        {
            g.setColour(juce::Colour(0xffefb986));
            g.drawVerticalLine(static_cast<int>(mapping.tickX(playhead)), 0, static_cast<float>(getHeight()));
        }
    }

private:
    studio::Scale scale() const { return { static_cast<float>(getWidth()), static_cast<int>(style.bars.size()) }; }

    const StyleTimeline& style;
    BarRange selection { 0, 0 };
    int selectedTrack = drumsTrack, playhead = -1;
};

class PianoRollKeys final : public juce::Component
{
public:
    void setScrollY(int y)
    {
        if (offset != y)
        {
            offset = y;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff333437));
        g.setFont(10.0f);
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            const int y = studio::Scale::pitchY(pitch) - offset;
            if (y < -studio::pitchHeight || y >= getHeight())
                continue;
            const bool black = studio::Scale::blackKey(pitch);
            g.setColour(juce::Colour(0xffc9c7c2));
            g.fillRect(0, y, getWidth(), studio::pitchHeight - 1);
            if (black)
            {
                g.setColour(juce::Colour(0xff303134));
                g.fillRect(0, y, getWidth() * 3 / 5, studio::pitchHeight - 1);
            }
            if (pitch % 12 == 0)
            {
                g.setColour(juce::Colour(0xff373538));
                g.drawText("C" + juce::String(pitch / 12 - 1), 5, y, getWidth() - 7, studio::pitchHeight - 1,
                           juce::Justification::centredLeft);
            }
        }
    }

private:
    int offset = 0;
};

class PianoRollRuler final : public juce::Component
{
public:
    explicit PianoRollRuler(const StyleTimeline& timeline) : style(timeline) {}

    void setScale(int timelineWidth, int scrollX)
    {
        width = timelineWidth;
        offset = scrollX;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff363638));
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(-offset), 0.0f));
        studio::paintTimeGrid(g, { static_cast<float>(width), static_cast<int>(style.bars.size()) },
                              getHeight() - 4, getHeight(), true);
    }

private:
    const StyleTimeline& style;
    int width = 1, offset = 0;
};
