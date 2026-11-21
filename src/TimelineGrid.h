#pragma once

#include "StyleTimeline.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace studio
{
inline constexpr std::array trackColours { 0xff579eaa, 0xffa473a1 };
inline constexpr int pitchHeight = 13;

struct Scale
{
    float width;
    int bars;

    float barWidth() const { return width / static_cast<float>(bars); }
    float tickX(int tick) const { return width * static_cast<float>(tick) / (bars * StyleTimeline::ticksPerBar); }
    int barAt(int x) const { return juce::jlimit(0, bars - 1, static_cast<int>(x / std::max(1.0f, barWidth()))); }
    static int pitchY(int pitch) { return (127 - pitch) * pitchHeight; }
    static bool blackKey(int pitch) { return (0x54au & (1u << (pitch % 12))) != 0; }
};

inline void paintTimeGrid(juce::Graphics& g, Scale scale, int top, int bottom, bool numbers)
{
    const auto clip = g.getClipBounds();
    const auto width = scale.barWidth();
    g.setFont(11.0f);
    for (int bar = scale.barAt(clip.getX()); bar <= scale.barAt(clip.getRight()); ++bar)
    {
        const auto x = static_cast<float>(bar) * width;
        for (int beat = 0; beat < 4; ++beat)
        {
            g.setColour(juce::Colour(beat == 0 ? 0xff56575a : 0xff343539));
            g.drawVerticalLine(static_cast<int>(x + width * beat / 4.0f),
                               static_cast<float>(top), static_cast<float>(bottom));
        }
        if (numbers)
        {
            g.setColour(juce::Colour(0xffced0d1));
            g.drawText(juce::String(bar + 1), static_cast<int>(x) + 6, 0, 38, top,
                       juce::Justification::centredLeft);
        }
    }
}

class SyncedViewport : public juce::Viewport
{
public:
    std::function<void(juce::Rectangle<int>)> onScroll;
    void visibleAreaChanged(const juce::Rectangle<int>& area) override
    {
        if (onScroll)
            onScroll(area);
    }
};
}

class TimelineGrid final : public juce::Component
{
public:
    explicit TimelineGrid(const StyleTimeline& timeline) : style(timeline)
    {
        setTitle("Arrangement timeline");
        setDescription("Drag across bars to select a recording or section range.");
        refresh();
    }

    static constexpr int laneHeight = 62;
    static constexpr int rulerHeight = 23;
    static constexpr int markerHeight = 16;
    static constexpr int minimumBarWidth = 66;
    int trackTop() const { return rulerHeight + markerRows * markerHeight + 3; }

    std::function<void(BarRange)> onSelection;
    std::function<void(int)> onSection;

    void refresh()
    {
        std::array<int, sectionCount> order {};
        std::array<int, sectionCount> rowEnds;
        rowEnds.fill(-1);
        for (int i = 0; i < sectionCount; ++i)
            order[static_cast<std::size_t>(i)] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b)
        {
            return style.assignments[static_cast<std::size_t>(a)].first
                 < style.assignments[static_cast<std::size_t>(b)].first;
        });
        markerRows = 1;
        for (auto section : order)
        {
            const auto index = static_cast<std::size_t>(section);
            const auto range = style.assignments[index];
            const auto available = std::find_if(rowEnds.begin(), rowEnds.end(), [&](int end)
            {
                return end < range.first;
            });
            const auto row = static_cast<int>(available - rowEnds.begin());
            markerRow[index] = row;
            *available = range.last;
            markerRows = std::max(markerRows, row + 1);
        }
        repaint();
    }

    void showSelection(BarRange range) { selection = range; repaint(); }
    void showAssignedSection(int section) { assignedSection = section; repaint(); }

    void showPlayhead(int absoluteTick)
    {
        if (playhead == absoluteTick)
            return;
        const auto redraw = [this](int tick)
        {
            if (tick >= 0)
                repaint(static_cast<int>(scale().tickX(tick)) - 5, 0, 11, getHeight());
        };
        redraw(playhead);
        playhead = absoluteTick;
        redraw(playhead);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!event.mods.isLeftButtonDown())
            return;

        const auto bar = scale().barAt(event.x);
        if (event.y >= rulerHeight && event.y < trackTop())
        {
            const int row = (event.y - rulerHeight) / markerHeight;
            for (int i = 0; i < sectionCount; ++i)
            {
                const auto idx = static_cast<std::size_t>(i);
                const auto region = style.assignments[idx];
                if (row == markerRow[idx] && bar >= region.first && bar <= region.last)
                {
                    if (onSection)
                        onSection(i);
                    return;
                }
            }
        }
        anchor = bar;
        select(bar);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (anchor >= 0)
            select(scale().barAt(event.x));
    }

    void mouseUp(const juce::MouseEvent&) override { anchor = -1; }

    void paint(juce::Graphics& g) override
    {
        const auto mapping = scale();
        const auto width = mapping.barWidth();
        const int top = trackTop();
        const auto clip = g.getClipBounds();
        const int first = mapping.barAt(clip.getX());
        const int last = mapping.barAt(clip.getRight());

        g.fillAll(juce::Colour(0xff222326));
        g.setColour(juce::Colour(0xff303134));
        g.fillRect(0, 0, getWidth(), top);
        g.setColour(juce::Colour(0xff292a2d));
        g.fillRect(0, top + laneHeight, getWidth(), laneHeight);
        g.setColour(juce::Colour(0xff5c7d77).withAlpha(0.16f));
        g.fillRect(selection.first * width, static_cast<float>(rulerHeight),
                   (selection.last - selection.first + 1) * width, static_cast<float>(getHeight() - rulerHeight));
        studio::paintTimeGrid(g, mapping, rulerHeight, getHeight(), true);

        for (int lane = 0; lane <= trackCount; ++lane)
        {
            g.setColour(juce::Colour(0xff535356));
            g.drawHorizontalLine(top + lane * laneHeight, 0, static_cast<float>(getWidth()));
        }
        if (clip.getY() < top)
            paintMarkers(g, width);
        if (clip.getBottom() > top)
        {
            for (int track = 0; track < trackCount; ++track)
                paintTrack(g, mapping, track, first, last);
            paintNotes(g, mapping);
        }

        if (playhead >= 0)
        {
            const auto x = static_cast<int>(mapping.tickX(playhead));
            g.setColour(juce::Colour(0xffefb986));
            g.drawVerticalLine(x, 0, static_cast<float>(getHeight()));
            juce::Path tip;
            tip.addTriangle(x - 4.0f, 1.0f, x + 4.0f, 1.0f, static_cast<float>(x), 7.0f);
            g.fillPath(tip);
        }
    }

private:
    studio::Scale scale() const { return { static_cast<float>(getWidth()), static_cast<int>(style.bars.size()) }; }

    void select(int bar) const
    {
        if (onSelection)
            onSelection({ std::min(anchor, bar), std::max(anchor, bar) });
    }

    void paintMarkers(juce::Graphics& g, float width) const
    {
        g.setFont(10.5f);
        for (std::size_t i = 0; i < style.assignments.size(); ++i)
        {
            const auto range = style.assignments[i];
            const juce::Rectangle<int> marker(static_cast<int>(range.first * width) + 1,
                rulerHeight + markerRow[i] * markerHeight + 1,
                static_cast<int>((range.last - range.first + 1) * width) - 2, markerHeight - 2);
            g.setColour(juce::Colour(static_cast<int>(i) == assignedSection ? 0xff8d694e : 0xff536666));
            g.fillRect(marker);
            g.setColour(juce::Colour(0xfff6eee7));
            g.drawFittedText(sectionNames[i], marker.reduced(5, 0), juce::Justification::centredLeft, 1);
        }
    }

    void paintTrack(juce::Graphics& g, studio::Scale mapping, int track, int first, int last) const
    {
        const auto accent = juce::Colour(studio::trackColours[static_cast<std::size_t>(track)]);
        const int top = trackTop() + track * laneHeight;
        const auto width = mapping.barWidth();
        int bar = first;
        while (bar > 0 && isRecorded(track, bar) && isRecorded(track, bar - 1))
            --bar;

        for (; bar <= last; ++bar)
        {
            const auto& pattern = style.bars[static_cast<std::size_t>(bar)];
            const auto hits = track == drumsTrack ? pattern.kick | pattern.snare | pattern.hat : pattern.bass;
            const auto recorded = isRecorded(track, bar);
            if (!recorded && hits == 0)
                continue;
            int end = bar;
            while (recorded && end + 1 < mapping.bars && isRecorded(track, end + 1))
                ++end;

            const auto x = width * bar;
            const juce::Rectangle<float> region(x + 1, static_cast<float>(top + 4), width * (end - bar + 1) - 2,
                                                static_cast<float>(laneHeight - 8));
            g.setColour(accent.withMultipliedBrightness(recorded ? 0.71f : 0.50f));
            g.fillRect(region);
            g.setColour(accent.withMultipliedBrightness(0.95f));
            g.fillRect(region.withHeight(17));
            g.setColour(juce::Colour(0xff16181a));
            g.setFont(11.0f);
            g.drawFittedText(recorded ? (track == drumsTrack ? "Drums MIDI" : "Bass MIDI")
                                      : (track == drumsTrack ? "Drums" : "Bass"),
                             region.withHeight(17).toNearestInt().reduced(5, 0), juce::Justification::centredLeft, 1);
            if (bar <= selection.last && end >= selection.first)
            {
                g.setColour(juce::Colour(0xffddd7c9).withAlpha(0.65f));
                g.drawRect(region, 1.0f);
            }

            if (!recorded)
            {
                if (track == drumsTrack)
                {
                    paintHits(g, x, width, top + 45, pattern.kick, accent);
                    paintHits(g, x, width, top + 36, pattern.snare, accent);
                    paintHits(g, x, width, top + 28, pattern.hat, accent.withAlpha(0.50f));
                }
                else
                    paintHits(g, x, width, top + 40, hits, accent);
            }
            bar = end;
        }
    }

    bool isRecorded(int track, int bar) const
    {
        return (style.bars[static_cast<std::size_t>(bar)].recordedTracks & (1u << track)) != 0;
    }

    static void paintHits(juce::Graphics& g, float x, float width, int y, unsigned mask, juce::Colour accent)
    {
        g.setColour(accent);
        for (int step = 0; step < 8; ++step)
            if ((mask & (1u << step)) != 0)
                g.fillRect(x + width * step / 8.0f + 2.0f, static_cast<float>(y), 4.0f, 2.0f);
    }

    void paintNotes(juce::Graphics& g, studio::Scale mapping) const
    {
        const auto clip = g.getClipBounds();
        for (const auto& note : style.notes)
        {
            const auto x = mapping.tickX(note.tick);
            const auto duration = std::max(1.0f, mapping.tickX(note.length));
            if (x + duration < clip.getX() || x >= clip.getRight())
                continue;
            const int y = trackTop() + note.track * laneHeight + 24 + (127 - note.pitch) * 28 / 128;
            g.setColour(juce::Colour(0xfff2e8e0).withAlpha(0.55f + 0.45f * note.velocity / 127.0f));
            g.fillRect(x, static_cast<float>(y), duration, 2.0f);
        }
    }

    const StyleTimeline& style;
    BarRange selection { 0, 0 };
    std::array<int, sectionCount> markerRow {};
    int markerRows = 1;
    int anchor = -1, assignedSection = mainASection, playhead = -1;
};
